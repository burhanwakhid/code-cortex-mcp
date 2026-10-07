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
#include "foundation/sha256.h"
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

static const char *nz(const char *p) {
    return p ? p : "";
}

static void set_slug(cbm_branch_id_t *out) {
    free(out->slug);
    out->slug = cbm_git_branch_slug(out->branch, out->is_detached);
}

void cbm_branch_id_free(cbm_branch_id_t *id) {
    if (id) {
        free(id->branch);
        free(id->slug);
        id->branch = NULL;
        id->slug = NULL;
        id->known = false;
    }
}

bool cbm_branch_id_indexed(cbm_store_t *s, const char *project, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    char *qn = NULL;
    char *props = NULL;
    if (cbm_store_get_branch_dup(s, project, &qn, NULL, &props) != CBM_STORE_OK) {
        return false;
    }
    yyjson_doc *doc = props ? yyjson_read(props, strlen(props), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *is_git = root ? yyjson_obj_get(root, "is_git") : NULL;
    yyjson_val *branch = root ? yyjson_obj_get(root, "branch") : NULL;
    if (yyjson_get_bool(is_git) && yyjson_is_str(branch)) {
        out->is_detached = yyjson_get_bool(yyjson_obj_get(root, "is_detached"));
        if (!out->is_detached) {
            out->branch = strdup(yyjson_get_str(branch));
        }
        yyjson_val *sha = yyjson_obj_get(root, "head_sha");
        out->has_sha = yyjson_is_str(sha);
        snprintf(out->head_sha, sizeof(out->head_sha), "%s",
                 out->has_sha ? yyjson_get_str(sha) : "");
        set_slug(out);
    } else if (yyjson_is_bool(is_git)) {
        /* Indexed as a non-git tree (or git refused the checkout, e.g.
         * safe.directory): there is no branch to compare, and comparing
         * "working-tree" with the checkout's branch would mark every session
         * pending. Leave it unknown. */
    } else {
        /* No properties (they failed to serialize): the QN's slug is all
         * there is. */
        const char *slug = cbm_branch_slug_from_qn(qn, project);
        out->slug = slug ? strdup(slug) : NULL;
        out->is_detached = slug && strcmp(slug, "detached") == 0;
    }
    yyjson_doc_free(doc);
    free(qn);
    free(props);
    out->known = nz(out->slug)[0] != '\0';
    return out->known;
}

void cbm_branch_id_from_context(const cbm_git_context_t *ctx, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->is_git) {
        return;
    }
    out->is_detached = ctx->is_detached;
    if (!ctx->is_detached && ctx->branch) {
        out->branch = strdup(ctx->branch);
    }
    out->has_sha = ctx->head_sha != NULL;
    snprintf(out->head_sha, sizeof(out->head_sha), "%s", nz(ctx->head_sha));
    set_slug(out);
    out->known = nz(out->slug)[0] != '\0';
}

bool cbm_branch_id_from_head(const char *root, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    cbm_git_head_t head;
    if (cbm_git_head_read(root, &head) != 0) {
        return false;
    }
    out->is_detached = head.is_detached;
    out->branch = head.branch; /* ownership moves */
    /* An unreadable ref and an unborn branch both leave head_sha empty; only
     * a commit actually read is compared. */
    out->has_sha = head.head_sha[0] != '\0';
    snprintf(out->head_sha, sizeof(out->head_sha), "%s", head.head_sha);
    set_slug(out);
    out->known = nz(out->slug)[0] != '\0';
    return out->known;
}

bool cbm_branch_id_same_strict(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current) {
    if (!current || !current->known) {
        return true;
    }
    if (!indexed || !indexed->known) {
        return false;
    }
    bool indexed_sha = indexed->has_sha && indexed->head_sha[0] != '\0';
    return cbm_branch_id_same(indexed, current) && (indexed_sha || current->head_sha[0] == '\0');
}

bool cbm_branch_id_same(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current) {
    if (!indexed || !current || !indexed->known || !current->known) {
        return true;
    }
    if (indexed->is_detached != current->is_detached) {
        return false;
    }
    if (!indexed->is_detached) {
        bool named = nz(indexed->branch)[0] && nz(current->branch)[0];
        if (named ? strcmp(indexed->branch, current->branch) != 0
                  : strcmp(nz(indexed->slug), nz(current->slug)) != 0) {
            return false;
        }
    }
    return !(indexed->has_sha && current->has_sha &&
             strcmp(indexed->head_sha, current->head_sha) != 0);
}

/* The slot file name for slug: the slug itself when it is short enough to be
 * one path component with room for every suffix this module and SQLite add
 * ("<name>.db.tmp-journal", "<name>.db-wal", ...). A longer slug -- a long or
 * deeply nested branch name, which git allows -- becomes a prefix of it plus
 * '-' and 16 hex digits of the SHA-256 of the whole slug, so distinct slugs
 * keep distinct slots and the same slug always maps to the same file. */
bool cbm_branch_slot_name(const char *slug, char *buf, size_t bufsz) {
    if (!slug || !buf || bufsz <= CBM_BRANCH_SLOT_NAME_MAX) {
        return false;
    }
    size_t len = strlen(slug);
    if (len <= CBM_BRANCH_SLOT_NAME_MAX) {
        snprintf(buf, bufsz, "%s", slug);
        return true;
    }
    enum { HASH_HEX = 16, PREFIX = CBM_BRANCH_SLOT_NAME_MAX - HASH_HEX - 1 };
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(slug, len, digest);
    snprintf(buf, bufsz, "%.*s-%.*s", (int)PREFIX, slug, (int)HASH_HEX, digest);
    return true;
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

bool cbm_branch_root_key(const char *root, char *buf, size_t bufsz) {
    char canon[CBM_ROOT_MAX];
    if (!buf || bufsz <= CBM_BRANCH_ROOT_KEY_LEN ||
        !cbm_root_canonical(root, canon, sizeof(canon))) {
        return false;
    }
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(canon, strlen(canon), digest);
    snprintf(buf, bufsz, "%.*s", (int)CBM_BRANCH_ROOT_KEY_LEN, digest);
    return true;
}

bool cbm_branch_slots_dir(const char *db_path, const char *project, const char *root, char *buf,
                          size_t bufsz) {
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
    char key[CBM_BRANCH_ROOT_KEY_LEN + 1] = "";
    if (root && root[0] && !cbm_branch_root_key(root, key, sizeof(key))) {
        return false;
    }
    int n =
        snprintf(buf, bufsz, "%.*s/branches/%s%s%s", dir_len, dir, project, key[0] ? "/" : "", key);
    if (n <= 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

static bool slot_path(const char *dir, const char *slug, char *buf, size_t bufsz) {
    char name[CBM_BRANCH_SLOT_NAME_MAX + 1];
    if (!cbm_branch_slot_name(slug, name, sizeof(name))) {
        return false;
    }
    int n = snprintf(buf, bufsz, "%s/%s.db", dir, name);
    return n > 0 && (size_t)n < bufsz;
}

static bool is_slot_db_name(const char *name) {
    size_t len = strlen(name);
    return len > BS_DB_EXT && strcmp(name + len - BS_DB_EXT, ".db") == 0;
}

/* The slug the database at db_path was built for, from its Branch node;
 * NULL when it records none. Caller frees. */
static char *read_indexed_slug(const char *db_path, const char *project) {
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return NULL;
    }
    cbm_branch_id_t id;
    bool ok = cbm_branch_id_indexed(s, project, &id);
    cbm_store_close(s);
    char *slug = NULL;
    if (ok && slug_is_safe(id.slug)) {
        slug = id.slug;
        id.slug = NULL;
    }
    cbm_branch_id_free(&id);
    return slug;
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

static int64_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static int64_t wal_size(const char *db_path) {
    char wal[BS_PATH_MAX];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    return file_size(wal);
}

/* One info line with two integer fields (timings and sizes the soak reads). */
static void log_bytes(const char *msg, const char *k1, int64_t v1, const char *k2, int64_t v2) {
    char b1[32];
    char b2[32];
    snprintf(b1, sizeof(b1), "%lld", (long long)v1);
    snprintf(b2, sizeof(b2), "%lld", (long long)v2);
    cbm_log_info(msg, k1, b1, k2, b2);
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
 * one transaction. False when the slot could not be brought in line -- and
 * when the outgoing ADR could not be read: the live database may then hold
 * the only copy of the newest ADR, so it must stay in place (the caller
 * keeps it as the seed) rather than be overwritten by the slot's older one. */
static bool carry_adr(const char *db_path, const char *project, const char *slot) {
    char *adr = NULL;
    adr_state_t state = read_adr(db_path, project, &adr);
    if (state == ADR_UNREADABLE) {
        cbm_log_warn("branch.adr_unreadable", "project", project);
        return false;
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

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*before_restore_cb)(void *) = nullptr;
static void *before_restore_data = nullptr;

void cbm_branch_slots_test_before_restore(void (*callback)(void *), void *data) {
    before_restore_cb = callback;
    before_restore_data = data;
}
#endif

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
 * until the next re-dump replaces the file).
 *
 * The ADR is carried INSIDE that transaction's write lock. The first backup
 * step takes db_path's write lock and holds it until the backup completes;
 * only then is the live ADR read (on another connection, which sees every
 * commit made before the lock) and written into the slot, and the rest of
 * the backup copies it. A manage_adr write from another process therefore
 * either committed before the lock and is carried, or waits on the lock and
 * lands on top of the restored database -- never lost in between. Writing
 * the slot restarts the copy (it is the backup's source), still under the
 * same lock.
 *
 * Fails, leaving db_path as it was, when the ADR cannot be carried, the page
 * sizes differ (a WAL database cannot change its page size) or the write lock
 * is not granted in time. */
static bool restore_into(const char *slot, const char *db_path, const char *project) {
#ifdef CBM_ENABLE_TEST_SEAMS
    if (before_restore_cb) {
        before_restore_cb(before_restore_data);
    }
#endif
    cbm_store_t *src = cbm_store_open_path_query(slot);
    cbm_store_t *dst = src ? cbm_store_open_path(db_path) : NULL;
    sqlite3 *sdb = cbm_store_get_db(src);
    sqlite3 *ddb = cbm_store_get_db(dst);
    bool ok = false;
    sqlite3_backup *bk = sdb && ddb ? sqlite3_backup_init(ddb, "main", sdb, "main") : NULL;
    if (bk) {
        int rc = sqlite3_backup_step(bk, 1);
        if (rc == SQLITE_OK) {
            /* Write lock held, nothing committed yet. */
            if (carry_adr(db_path, project, slot)) {
                rc = sqlite3_backup_step(bk, -1);
                ok = rc == SQLITE_DONE;
            }
        } else if (rc == SQLITE_DONE) {
            /* A one-page database: no index database is that small (its
             * schema alone spans several pages), so nothing to carry into. */
            cbm_log_warn("branch.restore_tiny", "project", project);
            ok = true;
        }
        /* Before SQLITE_DONE, finish rolls the destination back. */
        ok = sqlite3_backup_finish(bk) == SQLITE_OK && ok;
    }
    cbm_store_close(dst);
    cbm_store_close(src);
    return ok;
}

static cbm_branch_switch_t switch_from(const char *db_path, const char *project, const char *slug,
                                       const char *indexed, const char *dir, const char *incoming,
                                       bool have_incoming, int keep);

/* Whether the database at path was indexed from root (cbm_root_matches): a
 * derived project name can be another directory's, and so can its slots. A
 * database with no project row or no recorded root is accepted. */
static bool db_root_matches(const char *path, const char *project, const char *root) {
    if (!root || !root[0]) {
        return true;
    }
    cbm_store_t *s = cbm_store_open_path_query(path);
    cbm_project_t proj = {0};
    bool ok = true;
    if (s && cbm_store_get_project(s, project, &proj) == CBM_STORE_OK) {
        ok = cbm_root_matches(proj.root_path, root);
        cbm_project_free_fields(&proj);
    }
    cbm_store_close(s);
    if (!ok) {
        cbm_log_warn("branch.foreign_db", "path", path, "root", root);
    }
    return ok;
}

/* Earlier builds parked slots directly under branches/<project>/, shared by
 * every root with that derived name. Move such a slot into this root's
 * directory when it records this root; otherwise leave it (delete_project
 * and the index reset still find it). */
static bool adopt_legacy_slot(const char *db_path, const char *project, const char *root,
                              const char *slug, const char *dir, const char *incoming) {
    char legacy_dir[BS_PATH_MAX];
    char legacy[BS_PATH_MAX];
    if (!root || !root[0] ||
        !cbm_branch_slots_dir(db_path, project, NULL, legacy_dir, sizeof(legacy_dir)) ||
        !slot_path(legacy_dir, slug, legacy, sizeof(legacy)) || !cbm_file_exists(legacy)) {
        return false;
    }
    cbm_store_t *s = cbm_store_open_path_query(legacy);
    cbm_project_t proj = {0};
    bool mine = s && cbm_store_get_project(s, project, &proj) == CBM_STORE_OK && proj.root_path &&
                proj.root_path[0] && cbm_root_matches(proj.root_path, root);
    cbm_project_free_fields(&proj);
    cbm_store_close(s);
    if (!mine || !cbm_mkdir_p(dir, 0755)) {
        return false;
    }
    cbm_remove_db_sidecars(incoming);
    if (cbm_rename_replace(legacy, incoming) != 0) {
        return false;
    }
    cbm_remove_db_sidecars(legacy);
    return true;
}

cbm_branch_switch_t cbm_branch_slots_switch(const char *db_path, const char *project,
                                            const char *slug, const char *root, int keep,
                                            char **from) {
    if (from) {
        *from = NULL;
    }
    char dir[BS_PATH_MAX];
    char incoming[BS_PATH_MAX];
    if (keep < 1 || !slug_is_safe(slug) ||
        !cbm_branch_slots_dir(db_path, project, root, dir, sizeof(dir)) ||
        !slot_path(dir, slug, incoming, sizeof(incoming))) {
        return CBM_BRANCH_SWITCH_NONE;
    }
    bool have_incoming = cbm_file_exists(incoming);
    if (!have_incoming) {
        have_incoming = adopt_legacy_slot(db_path, project, root, slug, dir, incoming);
    }

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

    char *indexed = read_indexed_slug(db_path, project);
    if (!indexed) {
        /* No Branch node: a non-git index or an unreadable file. Leave it to
         * the normal routing, which rebuilds what it cannot use. */
        return CBM_BRANCH_SWITCH_NONE;
    }
    /* Slots live under this root's own directory, so every slot found there
     * is this checkout's. The live database is shared by name, though: one
     * from another directory (a derived-name collision) must not be parked
     * among this root's slots. It is left to the normal routing, which
     * reindexes over it as it always has. Checked only when a switch is due,
     * so a run on the same branch opens nothing extra. */
    cbm_branch_switch_t action = CBM_BRANCH_SWITCH_NONE;
    if (strcmp(indexed, slug) != 0 && db_root_matches(db_path, project, root)) {
        action = switch_from(db_path, project, slug, indexed, dir, incoming, have_incoming, keep);
    }
    if (from) {
        *from = indexed;
    } else {
        free(indexed);
    }
    return action;
}

/* The switch proper, once the database is known to be another branch's. */
static cbm_branch_switch_t switch_from(const char *db_path, const char *project, const char *slug,
                                       const char *indexed, const char *dir, const char *incoming,
                                       bool have_incoming, int keep) {
    char outgoing[BS_PATH_MAX];
    uint64_t t0 = cbm_now_ms();
    if (!slot_path(dir, indexed, outgoing, sizeof(outgoing)) || !cbm_mkdir_p(dir, 0755) ||
        !copy_db(db_path, outgoing)) {
        cbm_log_warn("branch.park_failed", "from", indexed, "to", slug, "action", "in_place");
        return CBM_BRANCH_SWITCH_IN_PLACE;
    }
    log_bytes("branch.park", "elapsed_ms", (int64_t)(cbm_now_ms() - t0), "slot_bytes",
              file_size(outgoing));

    /* Restore by content (restore_into). A failure at any step leaves the
     * outgoing database in place, already parked: it becomes the seed, as if
     * nothing had been parked for this branch. */
    cbm_branch_switch_t action = CBM_BRANCH_SWITCH_SEEDED;
    if (have_incoming) {
        t0 = cbm_now_ms();
        if (restore_into(incoming, db_path, project)) {
            action = CBM_BRANCH_SWITCH_RESTORED;
            log_bytes("branch.restore", "elapsed_ms", (int64_t)(cbm_now_ms() - t0), "wal_bytes",
                      wal_size(db_path));
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

/* Append the slot stems in dir, each prefixed with prefix, to *list. */
static void list_dir_slots(const char *dir, const char *prefix, char ***list, int *n, int *cap) {
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return;
    }
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir || !is_slot_db_name(ent->name)) {
            continue;
        }
        if (*n == *cap) {
            int next_cap = *cap ? *cap * 2 : 8;
            char **grown = (char **)realloc(*list, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            *list = grown;
            *cap = next_cap;
        }
        size_t plen = strlen(prefix);
        size_t len = strlen(ent->name) - BS_DB_EXT;
        char *slug = (char *)malloc(plen + len + 1);
        if (!slug) {
            break;
        }
        memcpy(slug, prefix, plen);
        memcpy(slug + plen, ent->name, len);
        slug[plen + len] = '\0';
        (*list)[(*n)++] = slug;
    }
    cbm_closedir(d);
}

/* The root-key subdirectories of a project's slot directory. */
static int list_root_dirs(const char *pdir, char ***out) {
    *out = NULL;
    cbm_dir_t *d = cbm_opendir(pdir);
    if (!d) {
        return 0;
    }
    int n = 0;
    int cap = 0;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (!ent->is_dir || ent->name[0] == '.') {
            continue;
        }
        if (n == cap) {
            int next_cap = cap ? cap * 2 : 4;
            char **grown = (char **)realloc(*out, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            *out = grown;
            cap = next_cap;
        }
        if (!((*out)[n] = strdup(ent->name))) {
            break;
        }
        n++;
    }
    cbm_closedir(d);
    return n;
}

int cbm_branch_slots_list(const char *db_path, const char *project, const char *root, char ***out) {
    if (out) {
        *out = NULL;
    }
    char dir[BS_PATH_MAX];
    if (!out || !cbm_branch_slots_dir(db_path, project, root, dir, sizeof(dir))) {
        return 0;
    }
    int cap = 0;
    int n = 0;
    char **list = NULL;
    list_dir_slots(dir, "", &list, &n, &cap);
    if (!root || !root[0]) {
        /* Every root's slots, as "<root-key>/<slot>" (plus legacy ones). */
        char **keys = NULL;
        int nk = list_root_dirs(dir, &keys);
        for (int i = 0; i < nk; i++) {
            char sub[BS_PATH_MAX];
            char prefix[CBM_DIRENT_NAME_MAX + 2];
            snprintf(sub, sizeof(sub), "%s/%s", dir, keys[i]);
            snprintf(prefix, sizeof(prefix), "%s/", keys[i]);
            list_dir_slots(sub, prefix, &list, &n, &cap);
            free(keys[i]);
        }
        free(keys);
    }
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

/* Delete the slot files in dir (this module's names only). */
static int delete_dir_slots(const char *dir) {
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
    return removed;
}

int cbm_branch_slots_delete_all(const char *db_path, const char *project) {
    char dir[BS_PATH_MAX];
    if (!cbm_branch_slots_dir(db_path, project, NULL, dir, sizeof(dir))) {
        return 0;
    }
    /* Every root's slots (a derived name can be several directories'), and
     * any legacy ones parked directly in the project directory. */
    int removed = delete_dir_slots(dir);
    char **keys = NULL;
    int nk = list_root_dirs(dir, &keys);
    for (int i = 0; i < nk; i++) {
        char sub[BS_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", dir, keys[i]);
        removed += delete_dir_slots(sub);
        (void)cbm_rmdir(sub);
        free(keys[i]);
    }
    free(keys);
    (void)cbm_rmdir(dir);
    /* The shared parent goes too once no project has parked anything. */
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)cbm_rmdir(dir);
    }
    return removed;
}
