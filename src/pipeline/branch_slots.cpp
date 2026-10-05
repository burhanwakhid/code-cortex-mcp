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
    } else {
        /* No git identity recorded (a non-git index, or properties that
         * failed to serialize): the QN's slug is all there is. */
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

/* Fold the WAL into the main file and truncate it, so the main file alone is
 * the whole database. A reader in the middle of a WAL read makes this return
 * false (SQLITE_BUSY); readers never write, so once it succeeds the WAL stays
 * empty for as long as the caller holds the index lease. */
static bool checkpoint_truncate(const char *db_path) {
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s) {
        return false;
    }
    sqlite3 *db = cbm_store_get_db(s);
    int log_frames = -1;
    int done_frames = -1;
    int rc = db ? sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log_frames,
                                            &done_frames)
                : SQLITE_ERROR;
    cbm_store_close(s);
    return rc == SQLITE_OK;
}

/* Copy db_path to dest through the SQLite backup API (a consistent snapshot,
 * WAL included) into dest.tmp, then rename over dest. */
static bool park_copy(const char *db_path, const char *dest) {
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return false;
    }
    int rc = cbm_store_dump_to_file(s, dest);
    cbm_store_close(s);
    return rc == CBM_STORE_OK;
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

/* Move a parked database into place. Only the rename is visible to readers:
 * db_path is replaced atomically or not at all. Its sidecars go first (the
 * #897 rule for installing a new generation), which is safe only because
 * checkpoint_truncate already made the main file complete — a failed rename
 * then leaves a whole database behind. */
static bool install_slot(const char *slot, const char *db_path) {
    cbm_remove_db_sidecars(db_path);
    if (cbm_rename_replace(slot, db_path) != 0) {
        return false;
    }
    cbm_remove_db_sidecars(slot);
    return true;
}

/* The project's ADR (project_summaries) in the database at db_path; NULL when
 * there is none. Caller frees. */
static char *read_adr(const char *db_path, const char *project) {
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return NULL;
    }
    char *content = NULL;
    cbm_adr_t adr = {0};
    if (cbm_store_adr_get(s, project, &adr) == CBM_STORE_OK) {
        content = adr.content ? strdup(adr.content) : NULL;
        cbm_store_adr_free(&adr);
    }
    cbm_store_close(s);
    return content;
}

/* The ADR is the one table in the database that is not derived from the
 * indexed source: manage_adr writes it for the project, whatever branch is
 * checked out. A restored database carries the ADR from when it was parked,
 * so put the outgoing (newest) one back. An outgoing database without an ADR
 * cannot say whether it was deleted or never set (a delete leaves no trace),
 * so then the restored database keeps its own. */
static void carry_adr(const char *db_path, const char *project, const char *adr) {
    if (!adr) {
        return;
    }
    char *restored = read_adr(db_path, project);
    bool same = restored && strcmp(restored, adr) == 0;
    free(restored);
    if (same) {
        return;
    }
    cbm_store_t *s = cbm_store_open_path(db_path);
    if (!s || cbm_store_adr_store(s, project, adr) != CBM_STORE_OK) {
        cbm_log_warn("branch.adr_carry_failed", "project", project);
    }
    cbm_store_close(s);
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

    /* No current database (first index here, or a full rebuild that died
     * after deleting it): a parked one for this branch is a better start than
     * nothing. Nothing is outgoing, so nothing needs parking. */
    if (!cbm_file_exists(db_path)) {
        if (have_incoming && install_slot(incoming, db_path)) {
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
        !checkpoint_truncate(db_path) || !park_copy(db_path, outgoing)) {
        cbm_log_warn("branch.park_failed", "from", indexed, "to", slug, "action", "in_place");
        return CBM_BRANCH_SWITCH_IN_PLACE;
    }

    cbm_branch_switch_t action = CBM_BRANCH_SWITCH_SEEDED;
    if (have_incoming) {
        char *adr = read_adr(db_path, project);
        if (install_slot(incoming, db_path)) {
            action = CBM_BRANCH_SWITCH_RESTORED;
            carry_adr(db_path, project, adr);
        } else {
            /* Windows refuses to replace a file another process has open. The
             * outgoing database is still in place and parked; carry on with
             * it as the seed, exactly as if nothing had been parked for this
             * branch. */
            cbm_log_warn("branch.restore_failed", "to", slug, "action", "seeded");
        }
        free(adr);
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
