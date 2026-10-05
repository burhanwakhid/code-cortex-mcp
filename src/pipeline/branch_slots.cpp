/*
 * branch_slots.cpp — Park and restore a project's database per git branch.
 * See branch_slots.h for the layout and the contract.
 */
#include "pipeline/branch_slots.h"

#include "store/store.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"

#include "sqlite3.h"
#include <yyjson/yyjson.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SLEN(s) (sizeof(s) - 1)

enum { BS_PATH_MAX = CBM_PATH_MAX, BS_DB_EXT = 3 /* strlen(".db") */ };

static const char BS_BRANCH_MARK[] = ".__branch__.";

const char *cbm_branch_switch_str(cbm_branch_switch_t action) {
    switch (action) {
    case CBM_BRANCH_SWITCH_RESTORED:
        return "restored";
    case CBM_BRANCH_SWITCH_SEEDED:
        return "seeded";
    case CBM_BRANCH_SWITCH_IN_PLACE:
        return "in_place";
    case CBM_BRANCH_SWITCH_NONE:
    default:
        return "none";
    }
}

const char *cbm_branch_slug_from_qn(const char *qn, const char *project) {
    if (!qn || !project) {
        return NULL;
    }
    size_t plen = strlen(project);
    if (strncmp(qn, project, plen) != 0 ||
        strncmp(qn + plen, BS_BRANCH_MARK, SLEN(BS_BRANCH_MARK)) != 0 ||
        !qn[plen + SLEN(BS_BRANCH_MARK)]) {
        return NULL;
    }
    return qn + plen + SLEN(BS_BRANCH_MARK);
}

static void set_slug(cbm_branch_id_t *out, const char *branch, bool detached) {
    char *slug = cbm_git_branch_slug(branch, detached);
    snprintf(out->slug, sizeof(out->slug), "%s", slug ? slug : "");
    free(slug);
}

bool cbm_branch_id_indexed(cbm_store_t *s, const char *project, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    char qn[BS_PATH_MAX];
    char props[CBM_SZ_4K];
    if (cbm_store_get_branch_props(s, project, qn, sizeof(qn), NULL, 0, props, sizeof(props)) !=
        CBM_STORE_OK) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(props, strlen(props), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *is_git = root ? yyjson_obj_get(root, "is_git") : NULL;
    yyjson_val *branch = root ? yyjson_obj_get(root, "branch") : NULL;
    if (yyjson_get_bool(is_git) && yyjson_is_str(branch)) {
        out->is_detached = yyjson_get_bool(yyjson_obj_get(root, "is_detached"));
        if (!out->is_detached) {
            snprintf(out->branch, sizeof(out->branch), "%s", yyjson_get_str(branch));
        }
        yyjson_val *sha = yyjson_obj_get(root, "head_sha");
        out->has_sha = yyjson_is_str(sha);
        snprintf(out->head_sha, sizeof(out->head_sha), "%s",
                 out->has_sha ? yyjson_get_str(sha) : "");
        set_slug(out, out->branch, out->is_detached);
    } else if (yyjson_is_bool(is_git)) {
        /* Indexed as a non-git tree (or git refused the checkout, e.g.
         * safe.directory): there is no branch to compare, and comparing
         * "working-tree" with the checkout's branch would mark every session
         * pending. Leave it unknown. */
    } else {
        /* No properties (they failed to serialize): the QN's slug is all
         * there is. */
        const char *slug = cbm_branch_slug_from_qn(qn, project);
        snprintf(out->slug, sizeof(out->slug), "%s", slug ? slug : "");
        out->is_detached = slug && strcmp(slug, "detached") == 0;
    }
    yyjson_doc_free(doc);
    out->known = out->slug[0] != '\0';
    return out->known;
}

void cbm_branch_id_from_context(const cbm_git_context_t *ctx, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->is_git) {
        return;
    }
    out->is_detached = ctx->is_detached;
    if (!ctx->is_detached) {
        snprintf(out->branch, sizeof(out->branch), "%s", ctx->branch ? ctx->branch : "");
    }
    out->has_sha = ctx->head_sha != NULL;
    snprintf(out->head_sha, sizeof(out->head_sha), "%s", ctx->head_sha ? ctx->head_sha : "");
    set_slug(out, out->branch, out->is_detached);
    out->known = out->slug[0] != '\0';
}

bool cbm_branch_id_from_head(const char *root, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    cbm_git_head_t head;
    if (cbm_git_head_read(root, &head) != 0) {
        return false;
    }
    out->is_detached = head.is_detached;
    snprintf(out->branch, sizeof(out->branch), "%s", head.branch);
    /* An unreadable ref and an unborn branch both leave head_sha empty; only
     * a commit actually read is compared. */
    out->has_sha = head.head_sha[0] != '\0';
    snprintf(out->head_sha, sizeof(out->head_sha), "%s", head.head_sha);
    set_slug(out, out->branch, out->is_detached);
    out->known = out->slug[0] != '\0';
    return out->known;
}

bool cbm_branch_id_same(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current) {
    if (!indexed || !current || !indexed->known || !current->known) {
        return true;
    }
    if (indexed->is_detached != current->is_detached) {
        return false;
    }
    if (!indexed->is_detached) {
        bool named = indexed->branch[0] && current->branch[0];
        if (named ? strcmp(indexed->branch, current->branch) != 0
                  : strcmp(indexed->slug, current->slug) != 0) {
            return false;
        }
    }
    return !(indexed->has_sha && current->has_sha &&
             strcmp(indexed->head_sha, current->head_sha) != 0);
}

/* A slug is one path component: the git_context slugger only emits
 * [A-Za-z0-9._-], but the stored QN is data, so check before building a path
 * from it. */
static bool slug_is_safe(const char *slug) {
    if (!slug || !slug[0] || strcmp(slug, ".") == 0 || strcmp(slug, "..") == 0) {
        return false;
    }
    for (const char *p = slug; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }
    return true;
}

bool cbm_branch_slots_dir(const char *db_path, const char *project, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) {
        return false;
    }
    buf[0] = '\0';
    if (!db_path || !project || !cbm_validate_project_name(project)) {
        return false;
    }
    const char *slash = strrchr(db_path, '/');
    const char *bslash = strrchr(db_path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    int dir_len = slash ? (int)(slash - db_path) : 1;
    const char *dir = slash ? db_path : ".";
    int n = snprintf(buf, bufsz, "%.*s/branches/%s", dir_len, dir, project);
    if (n <= 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

static bool slot_path(const char *dir, const char *slug, char *buf, size_t bufsz) {
    int n = snprintf(buf, bufsz, "%s/%s.db", dir, slug);
    return n > 0 && (size_t)n < bufsz;
}

static bool is_slot_db_name(const char *name) {
    size_t len = strlen(name);
    return len > BS_DB_EXT && strcmp(name + len - BS_DB_EXT, ".db") == 0;
}

/* The slug the database at db_path was built for, from its Branch node. */
static bool read_indexed_slug(const char *db_path, const char *project, char *buf, size_t bufsz) {
    buf[0] = '\0';
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return false;
    }
    cbm_branch_id_t id;
    bool ok = cbm_branch_id_indexed(s, project, &id);
    cbm_store_close(s);
    if (!ok || !slug_is_safe(id.slug)) {
        return false;
    }
    snprintf(buf, bufsz, "%s", id.slug);
    return true;
}

/* Copy the database at src to dest through the SQLite backup API: a read
 * transaction on src, so a consistent committed snapshot with its WAL frames
 * included, written to dest.tmp and renamed over dest after dest's own stale
 * sidecars are removed (cbm_store_dump_to_file, #897). src is only read --
 * no checkpoint, nothing done to its WAL, so readers of src are untouched.
 * dest must have no readers of its own: a slot, or a db_path that does not
 * exist. */
static bool copy_db(const char *src, const char *dest) {
    cbm_store_t *s = cbm_store_open_path_query(src);
    if (!s) {
        return false;
    }
    int rc = cbm_store_dump_to_file(s, dest);
    cbm_store_close(s);
    return rc == CBM_STORE_OK;
}

/* Replace the CONTENT of the live database at db_path with the slot's, as
 * one ordinary SQLite write transaction (backup API into db_path's own
 * connection). The file, its WAL and its shared memory stay in place: a
 * reader mid-query keeps its snapshot, the next read transaction sees the
 * whole restored database, a crash before the commit leaves the outgoing
 * database intact, and nothing is renamed over a file another process has
 * open. Swapping the file in instead would leave a window -- and after a
 * crash, a permanent state -- in which db_path's old WAL sits beside the new
 * main file and is replayed onto it. The price is the copy going through the
 * WAL (the slot's size written twice, and the WAL keeps that size on disk
 * until the next re-dump replaces the file). Fails, leaving db_path as it
 * was, when the page sizes differ (a WAL database cannot change its page
 * size) or the write lock is not granted in time. */
static bool restore_into(const char *slot, const char *db_path) {
    cbm_store_t *src = cbm_store_open_path_query(slot);
    cbm_store_t *dst = src ? cbm_store_open_path(db_path) : NULL;
    sqlite3 *sdb = cbm_store_get_db(src);
    sqlite3 *ddb = cbm_store_get_db(dst);
    bool ok = false;
    if (sdb && ddb) {
        sqlite3_backup *bk = sqlite3_backup_init(ddb, "main", sdb, "main");
        if (bk) {
            int rc = sqlite3_backup_step(bk, -1);
            ok = rc == SQLITE_DONE && sqlite3_backup_finish(bk) == SQLITE_OK;
            if (rc != SQLITE_DONE) {
                (void)sqlite3_backup_finish(bk);
            }
        }
    }
    cbm_store_close(dst);
    cbm_store_close(src);
    return ok;
}

typedef struct {
    char name[CBM_DIRENT_NAME_MAX];
    int64_t mtime;
} bs_entry_t;

static int64_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_mtime;
}

static int bs_entry_cmp_oldest(const void *a, const void *b) {
    const bs_entry_t *ea = (const bs_entry_t *)a;
    const bs_entry_t *eb = (const bs_entry_t *)b;
    if (ea->mtime != eb->mtime) {
        return ea->mtime < eb->mtime ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

static void unlink_slot(const char *path) {
    (void)cbm_unlink(path);
    cbm_remove_db_sidecars(path);
}

/* Keep at most `keep` parked databases: drop the least recently parked. */
static void evict_slots(const char *dir, int keep) {
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return;
    }
    int cap = 16;
    int n = 0;
    bs_entry_t *items = (bs_entry_t *)malloc((size_t)cap * sizeof(bs_entry_t));
    cbm_dirent_t *ent;
    while (items && (ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir || !is_slot_db_name(ent->name)) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            bs_entry_t *grown = (bs_entry_t *)realloc(items, (size_t)cap * sizeof(bs_entry_t));
            if (!grown) {
                break;
            }
            items = grown;
        }
        char path[BS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->name);
        snprintf(items[n].name, sizeof(items[n].name), "%s", ent->name);
        items[n].mtime = file_mtime(path);
        n++;
    }
    cbm_closedir(d);
    if (!items) {
        return;
    }
    if (n > keep) {
        qsort(items, (size_t)n, sizeof(bs_entry_t), bs_entry_cmp_oldest);
        for (int i = 0; i < n - keep; i++) {
            char path[BS_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, items[i].name);
            unlink_slot(path);
            cbm_log_info("branch.evict", "slot", items[i].name);
        }
    }
    free(items);
}

typedef enum { ADR_ABSENT = 0, ADR_PRESENT, ADR_UNREADABLE } adr_state_t;

/* The project's ADR (project_summaries) in the database at db_path. *content
 * (caller frees) is set only for ADR_PRESENT. */
static adr_state_t read_adr(const char *db_path, const char *project, char **content) {
    *content = NULL;
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return ADR_UNREADABLE;
    }
    cbm_adr_t adr = {0};
    int rc = cbm_store_adr_get(s, project, &adr);
    cbm_store_close(s);
    if (rc == CBM_STORE_NOT_FOUND) {
        return ADR_ABSENT;
    }
    if (rc != CBM_STORE_OK || !adr.content || !(*content = strdup(adr.content))) {
        cbm_store_adr_free(&adr);
        return ADR_UNREADABLE;
    }
    cbm_store_adr_free(&adr);
    return ADR_PRESENT;
}

/* The ADR is the one table in the database that is not derived from the
 * indexed source: manage_adr writes it for the project, whatever branch is
 * checked out, so the outgoing live database is the authority. Write its
 * state -- the ADR, or its absence (a delete leaves no row) -- into the slot
 * about to be restored, before the restore, so the restore installs both in
 * one transaction. Only when the outgoing database could not be read does
 * the slot keep its own. False when the slot could not be brought in line. */
static bool carry_adr(const char *db_path, const char *project, const char *slot) {
    char *adr = NULL;
    adr_state_t state = read_adr(db_path, project, &adr);
    if (state == ADR_UNREADABLE) {
        cbm_log_warn("branch.adr_unreadable", "project", project);
        return true;
    }
    char *parked = NULL;
    adr_state_t parked_state = read_adr(slot, project, &parked);
    bool same = parked_state == state && (state == ADR_ABSENT || strcmp(parked, adr) == 0);
    free(parked);
    bool ok = same;
    if (!same) {
        cbm_store_t *s = cbm_store_open_path(slot);
        int rc = !s                     ? CBM_STORE_ERR
                 : state == ADR_PRESENT ? cbm_store_adr_store(s, project, adr)
                                        : cbm_store_adr_delete(s, project);
        ok = rc == CBM_STORE_OK || (state == ADR_ABSENT && rc == CBM_STORE_NOT_FOUND);
        cbm_store_close(s);
    }
    free(adr);
    return ok;
}

cbm_branch_switch_t cbm_branch_slots_switch(const char *db_path, const char *project,
                                            const char *slug, int keep, char *from,
                                            size_t from_sz) {
    if (from && from_sz > 0) {
        from[0] = '\0';
    }
    char dir[BS_PATH_MAX];
    char incoming[BS_PATH_MAX];
    if (keep < 1 || !slug_is_safe(slug) ||
        !cbm_branch_slots_dir(db_path, project, dir, sizeof(dir)) ||
        !slot_path(dir, slug, incoming, sizeof(incoming))) {
        return CBM_BRANCH_SWITCH_NONE;
    }
    bool have_incoming = cbm_file_exists(incoming);

    /* No current database (first index here, an interrupted full rebuild, a
     * manual delete): this branch's parked database is a better start than
     * nothing, and the incremental pass corrects whatever moved since it was
     * parked. With no main file there is no reader to protect, so it is
     * copied into place the way a fresh database is installed (stale
     * sidecars removed, then one rename). */
    if (!cbm_file_exists(db_path)) {
        if (have_incoming && copy_db(incoming, db_path)) {
            unlink_slot(incoming);
            cbm_log_info("branch.switch", "action", "restored", "to", slug, "from", "");
            return CBM_BRANCH_SWITCH_RESTORED;
        }
        return CBM_BRANCH_SWITCH_NONE;
    }

    char indexed[BS_PATH_MAX];
    if (!read_indexed_slug(db_path, project, indexed, sizeof(indexed))) {
        /* No Branch node: a non-git index or an unreadable file. Leave it to
         * the normal routing, which rebuilds what it cannot use. */
        return CBM_BRANCH_SWITCH_NONE;
    }
    if (from && from_sz > 0) {
        snprintf(from, from_sz, "%s", indexed);
    }
    if (strcmp(indexed, slug) == 0) {
        return CBM_BRANCH_SWITCH_NONE;
    }

    char outgoing[BS_PATH_MAX];
    if (!slot_path(dir, indexed, outgoing, sizeof(outgoing)) || !cbm_mkdir_p(dir, 0755) ||
        !copy_db(db_path, outgoing)) {
        cbm_log_warn("branch.park_failed", "from", indexed, "to", slug, "action", "in_place");
        return CBM_BRANCH_SWITCH_IN_PLACE;
    }

    /* Restore by content (restore_into). A failure at any step leaves the
     * outgoing database in place, already parked: it becomes the seed, as if
     * nothing had been parked for this branch. */
    cbm_branch_switch_t action = CBM_BRANCH_SWITCH_SEEDED;
    if (have_incoming) {
        if (carry_adr(db_path, project, incoming) && restore_into(incoming, db_path)) {
            action = CBM_BRANCH_SWITCH_RESTORED;
            unlink_slot(incoming);
        } else {
            cbm_log_warn("branch.restore_failed", "to", slug, "action", "seeded");
        }
    }
    evict_slots(dir, keep);
    cbm_log_info("branch.switch", "action", cbm_branch_switch_str(action), "from", indexed, "to",
                 slug);
    return action;
}

int cbm_branch_slots_list(const char *db_path, const char *project, char ***out) {
    if (out) {
        *out = NULL;
    }
    char dir[BS_PATH_MAX];
    if (!out || !cbm_branch_slots_dir(db_path, project, dir, sizeof(dir))) {
        return 0;
    }
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return 0;
    }
    int cap = 0;
    int n = 0;
    char **list = NULL;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir || !is_slot_db_name(ent->name)) {
            continue;
        }
        if (n == cap) {
            int next_cap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(list, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            list = grown;
            cap = next_cap;
        }
        size_t len = strlen(ent->name) - BS_DB_EXT;
        char *slug = (char *)malloc(len + 1);
        if (!slug) {
            break;
        }
        memcpy(slug, ent->name, len);
        slug[len] = '\0';
        list[n++] = slug;
    }
    cbm_closedir(d);
    if (n > 1) {
        qsort(list, (size_t)n, sizeof(char *), [](const void *a, const void *b) {
            return strcmp(*(const char *const *)a, *(const char *const *)b);
        });
    }
    *out = list;
    return n;
}

void cbm_branch_slots_free_list(char **list, int count) {
    for (int i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

int cbm_branch_slots_delete_all(const char *db_path, const char *project) {
    char dir[BS_PATH_MAX];
    if (!cbm_branch_slots_dir(db_path, project, dir, sizeof(dir))) {
        return 0;
    }
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return 0;
    }
    /* Collect first: unlinking while iterating is unspecified on some
     * platforms' readdir. */
    int removed = 0;
    int cap = 0;
    int n = 0;
    char **names = NULL;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir) {
            continue;
        }
        if (n == cap) {
            int next_cap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(names, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            names = grown;
            cap = next_cap;
        }
        size_t len = strlen(ent->name);
        names[n] = (char *)malloc(len + 1);
        if (!names[n]) {
            break;
        }
        memcpy(names[n], ent->name, len + 1);
        n++;
    }
    cbm_closedir(d);
    for (int i = 0; i < n; i++) {
        /* Only what this module writes: <slug>.db, its -wal/-shm, and the
         * dump's <slug>.db.tmp. Anything else in the directory is left. */
        const char *name = names[i];
        size_t len = strlen(name);
        bool is_db = is_slot_db_name(name);
        bool is_side =
            strstr(name, ".db-wal") || strstr(name, ".db-shm") ||
            (len > SLEN(".db.tmp") && strcmp(name + len - SLEN(".db.tmp"), ".db.tmp") == 0);
        if (is_db || is_side) {
            char path[BS_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, name);
            if (cbm_unlink(path) == 0 && is_db) {
                removed++;
            }
        }
        free(names[i]);
    }
    free(names);
    (void)cbm_rmdir(dir);
    /* The shared parent goes too once no project has parked anything. */
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)cbm_rmdir(dir);
    }
    return removed;
}
