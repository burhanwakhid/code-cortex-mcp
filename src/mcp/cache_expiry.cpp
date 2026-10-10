/* cache_expiry.cpp — Remove index databases nobody uses. See cache_expiry.h
 * and docs/CACHE_EXPIRY.md. */
#include "mcp/cache_expiry.h"

#include "cli/cli.h" /* cbm_config_*, CBM_CONFIG_CACHE_EXPIRY* */
#include "foundation/compat_fs.h"
#include "foundation/constants.h" /* CBM_INDEX_BUSY */
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "mcp/store_meta.h"
#include "pipeline/branch_slots.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum { CE_PATH_MAX = 4096, CE_BUSY_MS = 200 };

/* SQLite sidecars of <x>.db, and the temp files a dump (<x>.db.tmp) or an
 * artifact import (<x>.db.import_tmp) write before renaming into place. */
static const char *const CE_SIDECAR_SUFFIXES[] = {".db-wal", ".db-shm", ".db-journal",
                                                  ".db.tmp", ".db.import_tmp"};

/* ── Small helpers ──────────────────────────────────────────────── */

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n > m && strcmp(s + n - m, suffix) == 0;
}

static int64_t path_mtime(const char *path) {
    cbm_stat_t st;
    return cbm_stat(path, &st) == 0 ? (int64_t)st.st_mtime : -1;
}

static int64_t path_size(const char *path) {
    cbm_stat_t st;
    return cbm_stat(path, &st) == 0 ? (int64_t)st.st_size : 0;
}

static std::string fmt_days(int64_t s) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", (long long)(s / CBM_CACHE_DAY_S));
    return b;
}

static std::string fmt_time(int64_t t) {
    char b[32];
    time_t tt = (time_t)t;
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    strftime(b, sizeof(b), "%Y-%m-%d", &tm);
    return b;
}

/* How a walker that sizes or deletes treats one path. A link is never
 * followed: cbm_is_dir calls a junction or a directory symlink a directory,
 * and descending into one would size, or delete, whatever it points at --
 * outside the cache, or round a cycle. Asked of the path, not a dirent, so
 * it also covers the entry a caller hands in (the trash entry itself). */
typedef enum { WALK_FILE, WALK_DIR, WALK_LINK } walk_kind_t;

static walk_kind_t walk_kind(const char *path) {
    if (cbm_is_link(path)) {
        return WALK_LINK;
    }
    return cbm_is_dir(path) ? WALK_DIR : WALK_FILE;
}

/* Every file under dir, recursively, without following links. */
static int64_t tree_bytes(const char *dir) {
    if (walk_kind(dir) != WALK_DIR) {
        return 0;
    }
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return 0;
    }
    int64_t total = 0;
    std::vector<std::string> subdirs;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (!strcmp(ent->name, ".") || !strcmp(ent->name, "..")) {
            continue;
        }
        std::string p = std::string(dir) + "/" + ent->name;
        walk_kind_t k = walk_kind(p.c_str());
        if (k == WALK_DIR) {
            subdirs.push_back(p);
        } else if (k == WALK_FILE) {
            total += path_size(p.c_str());
        }
    }
    cbm_closedir(d);
    for (const auto &s : subdirs) {
        total += tree_bytes(s.c_str());
    }
    return total;
}

/* Remove a link itself: a directory junction or directory symlink on
 * Windows goes with rmdir (RemoveDirectoryW), anything else with unlink. */
static void remove_link(const char *path) {
    if (cbm_rmdir(path) != 0) {
        (void)cbm_unlink(path);
    }
}

/* Delete dir and everything under it. A link, dir itself included, is
 * removed and never followed. */
static void remove_tree(const char *dir) {
    walk_kind_t self = walk_kind(dir);
    if (self != WALK_DIR) {
        if (self == WALK_LINK) {
            remove_link(dir);
        } else {
            (void)cbm_unlink(dir);
        }
        return;
    }
    cbm_dir_t *d = cbm_opendir(dir);
    if (d) {
        std::vector<std::string> items;
        cbm_dirent_t *ent;
        while ((ent = cbm_readdir(d)) != NULL) {
            if (strcmp(ent->name, ".") != 0 && strcmp(ent->name, "..") != 0) {
                items.emplace_back(std::string(dir) + "/" + ent->name);
            }
        }
        cbm_closedir(d);
        for (const auto &it : items) {
            remove_tree(it.c_str());
        }
    }
    (void)cbm_rmdir(dir);
}

/* ── State in _config.db ────────────────────────────────────────── */

static sqlite3 *state_open(bool create_tables) {
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir || !cdir[0]) {
        return NULL;
    }
    char path[CE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/_config.db", cdir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, CE_BUSY_MS);
    if (create_tables &&
        sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS cache_usage ("
                     "db_name TEXT PRIMARY KEY,"
                     "last_used INTEGER NOT NULL DEFAULT 0,"
                     "root_path TEXT NOT NULL DEFAULT '',"
                     "missing_since INTEGER NOT NULL DEFAULT 0,"
                     "missing_checks INTEGER NOT NULL DEFAULT 0);"
                     "CREATE TABLE IF NOT EXISTS cache_sweep ("
                     "key TEXT PRIMARY KEY, value TEXT NOT NULL);",
                     NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

typedef struct {
    bool found;
    int64_t last_used;
    std::string root;
    int64_t missing_since;
    int missing_checks;
} usage_row_t;

static usage_row_t usage_get(sqlite3 *db, const char *key) {
    usage_row_t r = {false, 0, "", 0, 0};
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT last_used, root_path, missing_since, missing_checks "
                           "FROM cache_usage WHERE db_name = ?1;",
                           -1, &st, NULL) != SQLITE_OK) {
        return r;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r.found = true;
        r.last_used = sqlite3_column_int64(st, 0);
        const char *root = (const char *)sqlite3_column_text(st, 1);
        r.root = root ? root : "";
        r.missing_since = sqlite3_column_int64(st, 2);
        r.missing_checks = sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
    return r;
}

static bool usage_put(sqlite3 *db, const char *key, const usage_row_t &r) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO cache_usage (db_name, last_used, root_path, missing_since, "
                           "missing_checks) VALUES (?1, ?2, ?3, ?4, ?5) "
                           "ON CONFLICT(db_name) DO UPDATE SET last_used = excluded.last_used, "
                           "root_path = excluded.root_path, missing_since = "
                           "excluded.missing_since, missing_checks = excluded.missing_checks;",
                           -1, &st, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, r.last_used);
    sqlite3_bind_text(st, 3, r.root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, r.missing_since);
    sqlite3_bind_int(st, 5, r.missing_checks);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static void usage_forget(sqlite3 *db, const char *key) {
    sqlite3_stmt *st = NULL;
    if (db && sqlite3_prepare_v2(db, "DELETE FROM cache_usage WHERE db_name = ?1;", -1, &st,
                                 NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

static std::string sweep_get(sqlite3 *db, const char *key) {
    std::string v;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM cache_sweep WHERE key = ?1;", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0);
            v = t ? t : "";
        }
    }
    sqlite3_finalize(st);
    return v;
}

static void sweep_put(sqlite3 *db, const char *key, const std::string &value) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO cache_sweep (key, value) VALUES (?1, ?2);",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* ── Configuration ──────────────────────────────────────────────── */

void cbm_cache_expiry_cfg_load(cbm_cache_expiry_cfg_t *out, struct cbm_config *cfg) {
    out->enabled = true;
    out->unused_s = (int64_t)CBM_CACHE_EXPIRY_UNUSED_DAYS * CBM_CACHE_DAY_S;
    out->root_grace_s = (int64_t)CBM_CACHE_EXPIRY_ROOT_GRACE_DAYS * CBM_CACHE_DAY_S;
    out->root_checks = CBM_CACHE_EXPIRY_ROOT_CHECKS;
    out->slot_s = (int64_t)CBM_CACHE_EXPIRY_SLOT_DAYS * CBM_CACHE_DAY_S;
    out->trash_s = CBM_CACHE_EXPIRY_TRASH_S;
    out->scratch_s = CBM_CACHE_EXPIRY_SCRATCH_S;
    out->interval_s = CBM_CACHE_EXPIRY_INTERVAL_S;
    cbm_config_t *owned = NULL;
    if (!cfg) {
        const char *dir = cbm_resolve_cache_dir();
        owned = dir ? cbm_config_open(dir) : NULL;
        cfg = owned;
    }
    if (cfg) {
        out->enabled = cbm_config_get_bool(cfg, CBM_CONFIG_CACHE_EXPIRY, true);
        out->unused_s = (int64_t)cbm_config_get_int(cfg, CBM_CONFIG_CACHE_EXPIRY_UNUSED_DAYS,
                                                    CBM_CACHE_EXPIRY_UNUSED_DAYS) *
                        CBM_CACHE_DAY_S;
        out->root_grace_s =
            (int64_t)cbm_config_get_int(cfg, CBM_CONFIG_CACHE_EXPIRY_ROOT_GRACE_DAYS,
                                        CBM_CACHE_EXPIRY_ROOT_GRACE_DAYS) *
            CBM_CACHE_DAY_S;
        out->slot_s = (int64_t)cbm_config_get_int(cfg, CBM_CONFIG_CACHE_EXPIRY_SLOT_DAYS,
                                                  CBM_CACHE_EXPIRY_SLOT_DAYS) *
                      CBM_CACHE_DAY_S;
    }
    cbm_config_close(owned);
    const char *env = getenv("CBM_CACHE_EXPIRY");
    if (env && strcmp(env, "0") == 0) {
        out->enabled = false;
    }
}

/* ── Usage ──────────────────────────────────────────────────────── */

bool cbm_cache_usage_set(const char *db_path, int64_t last_used, const char *root_path) {
    const char *key = cbm_store_meta_key(db_path);
    sqlite3 *db = key[0] ? state_open(true) : NULL;
    if (!db) {
        return false;
    }
    usage_row_t r = usage_get(db, key);
    if (last_used > 0) {
        r.last_used = last_used;
    }
    if (root_path) {
        r.root = root_path;
    }
    r.missing_since = 0;
    r.missing_checks = 0;
    bool ok = usage_put(db, key, r);
    sqlite3_close(db);
    return ok;
}

void cbm_cache_usage_touch(const char *db_path, const char *root_path) {
    const char *key = cbm_store_meta_key(db_path);
    if (!key[0]) {
        return;
    }
    int64_t now = (int64_t)time(NULL);
    static std::mutex mu;
    static std::unordered_map<std::string, int64_t> seen;
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = seen.find(key);
        if (!root_path && it != seen.end() && now - it->second < CBM_CACHE_USAGE_RESOLUTION_S) {
            return;
        }
    }
    /* Read first: a hook process lands here once per database it opens, and
     * a read never takes _config.db's write lock. */
    sqlite3 *db = state_open(true);
    if (!db) {
        return;
    }
    usage_row_t r = usage_get(db, key);
    bool fresh = r.found && now - r.last_used < CBM_CACHE_USAGE_RESOLUTION_S;
    bool root_known = !root_path || (r.root == root_path && r.missing_checks == 0);
    bool ok = true;
    if (!fresh || !root_known) {
        r.last_used = now;
        if (root_path) {
            r.root = root_path;
            r.missing_since = 0;
            r.missing_checks = 0;
        }
        ok = usage_put(db, key, r);
    }
    sqlite3_close(db);
    /* Throttle only what reached _config.db: a write lost to a busy database
     * (a sweep holding its transaction) is retried on the next touch. */
    if (ok) {
        std::lock_guard<std::mutex> g(mu);
        seen[key] = now;
    }
}

/* ── Root state ─────────────────────────────────────────────────── */

typedef enum { ROOT_PRESENT, ROOT_MISSING, ROOT_UNCERTAIN } root_state_t;

/* Directories under which a missing path may only be an unmounted volume. */
static bool is_mount_parent(const std::string &dir) {
    static const char *const roots[] = {"/",        "/Volumes",       "/mnt",
                                        "/media",   "/run/media",     "/net",
                                        "/Network", "/System/Volumes", "/private/var/automount",
                                        "/afs",     "/cifs",          "/nfs"};
    for (const char *r : roots) {
        if (dir == r) {
            return true;
        }
    }
    /* /media/<user>, /run/media/<user> */
    for (const char *r : {"/media/", "/run/media/"}) {
        size_t n = strlen(r);
        if (dir.compare(0, n, r) == 0 && dir.find('/', n) == std::string::npos) {
            return true;
        }
    }
#ifdef _WIN32
    if (dir.size() <= 3 && dir.size() >= 2 && dir[1] == ':') {
        return true; /* a drive root */
    }
#endif
    return false;
}

/* Whether the checkout root recorded for a database exists. MISSING only
 * when the root is gone but a parent directory below any mount point still
 * exists: a removed worktree or a deleted clone. An absolute path whose
 * nearest existing ancestor is "/" or a mount parent (/Volumes, /mnt, ...)
 * may be an unmounted volume, and any error other than "no such file" (a
 * permission error, a network mount timing out) proves nothing: UNCERTAIN. */
static root_state_t root_state(const std::string &root) {
    bool absolute = !root.empty() && root[0] == '/';
#ifdef _WIN32
    absolute = absolute || (root.size() > 2 && root[1] == ':');
#endif
    if (!absolute) {
        return ROOT_UNCERTAIN;
    }
    /* cbm_stat, not stat: on Windows the narrow call cannot name a root with
     * characters outside the ANSI code page or past MAX_PATH, and its
     * "no such file" would count a live checkout as missing. */
    cbm_stat_t st;
    if (cbm_stat(root.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) ? ROOT_PRESENT : ROOT_MISSING;
    }
    if (errno != ENOENT && errno != ENOTDIR) {
        return ROOT_UNCERTAIN;
    }
    std::string dir = root;
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    for (;;) {
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos) {
            return ROOT_UNCERTAIN;
        }
        dir = slash == 0 ? "/" : dir.substr(0, slash);
        if (cbm_stat(dir.c_str(), &st) == 0) {
            return is_mount_parent(dir) ? ROOT_UNCERTAIN : ROOT_MISSING;
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            return ROOT_UNCERTAIN;
        }
        if (dir == "/") {
            return ROOT_UNCERTAIN;
        }
    }
}

/* The root a database records, without the store layer: the memo row at the
 * file's current generation, else one read of its projects table. */
static std::string learn_root(const char *path, const char *project) {
    cbm_file_gen_t gen;
    if (cbm_file_generation(path, &gen)) {
        cbm_store_meta_db_t *meta = cbm_store_meta_open();
        cbm_store_meta_row_t row;
        bool hit = cbm_store_meta_get(meta, path, &gen, &row) && row.root_path[0];
        cbm_store_meta_close(meta);
        if (hit) {
            return row.root_path;
        }
    }
    std::string root;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_busy_timeout(db, CE_BUSY_MS);
        const char *sqls[] = {"SELECT root_path FROM projects WHERE name = ?1;",
                              "SELECT root_path FROM projects LIMIT 1;"};
        for (const char *sql : sqls) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                if (strstr(sql, "?1")) {
                    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
                }
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const char *t = (const char *)sqlite3_column_text(st, 0);
                    root = t ? t : "";
                }
            }
            sqlite3_finalize(st);
            if (!root.empty()) {
                break;
            }
        }
    }
    sqlite3_close(db);
    return root;
}

/* The project a database file holds: its one primary `projects` row, the
 * rule the query-time fallback scan uses, else the filename stem. A legacy
 * or renamed file is served under that internal name. */
static std::string learn_project(const char *path, const std::string &stem) {
    cbm_file_gen_t gen;
    if (cbm_file_generation(path, &gen)) {
        cbm_store_meta_db_t *meta = cbm_store_meta_open();
        cbm_store_meta_row_t row;
        bool hit = cbm_store_meta_get(meta, path, &gen, &row) && row.project[0];
        cbm_store_meta_close(meta);
        if (hit && cbm_validate_project_name(row.project)) {
            return row.project;
        }
    }
    std::string name;
    int rows = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_busy_timeout(db, CE_BUSY_MS);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT name FROM projects WHERE instr(name, '::') = 0 LIMIT 2;",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *t = (const char *)sqlite3_column_text(st, 0);
                name = t ? t : "";
                rows++;
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return rows == 1 && cbm_validate_project_name(name.c_str()) ? name : stem;
}

/* ── Removal ────────────────────────────────────────────────────── */

int cbm_cache_remove_project(const char *db_path, const char *project, const char *trash_entry,
                             int *parked) {
    if (parked) {
        *parked = 0;
    }
    char wal[CE_PATH_MAX];
    char shm[CE_PATH_MAX];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    bool exists = cbm_file_exists(db_path);
    int rc = exists ? 0 : 1;
    if (exists && trash_entry) {
        char dst[CE_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%s.db", trash_entry, project);
        if (!cbm_mkdir_p(trash_entry, 0755) || cbm_rename_replace(db_path, dst) != 0) {
            rc = -1;
        } else if (cbm_file_exists(wal)) {
            snprintf(dst, sizeof(dst), "%s/%s.db-wal", trash_entry, project);
            (void)cbm_rename_replace(wal, dst);
        }
        (void)cbm_unlink(shm);
    } else if (exists) {
        if (cbm_unlink(db_path) != 0) {
            rc = -1;
        }
        int err = errno;
        (void)cbm_unlink(wal);
        (void)cbm_unlink(shm);
        errno = err; /* the caller reports the main file's error */
    }
    if (rc == -1) {
        return rc; /* the project still exists, and its parked branches with it */
    }
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    cbm_store_meta_forget(meta, db_path);
    cbm_store_meta_close(meta);
    sqlite3 *db = state_open(false);
    usage_forget(db, cbm_store_meta_key(db_path));
    sqlite3_close(db);
    char **slots = NULL;
    int n = cbm_branch_slots_list(db_path, project, NULL, &slots);
    cbm_branch_slots_free_list(slots, n);
    char sdir[CE_PATH_MAX];
    if (trash_entry && n > 0 && cbm_branch_slots_dir(db_path, project, NULL, sdir, sizeof(sdir))) {
        char dst[CE_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/branches", trash_entry);
        if (cbm_mkdir_p(trash_entry, 0755) && cbm_rename_replace(sdir, dst) == 0) {
            if (parked) {
                *parked = n;
            }
            return rc;
        }
    }
    int removed = cbm_branch_slots_delete_all(db_path, project);
    if (parked) {
        *parked = removed;
    }
    return rc;
}

/* A connection's PASSIVE checkpoint, which waits for and blocks nobody:
 * true when every committed frame is in the main file, so the file alone is
 * the whole database and nothing holds a snapshot that needs the WAL. A
 * reader in a transaction keeps frames un-copied: the database is in use. */
static bool quiesced(const char *path) {
    sqlite3 *db = NULL;
    bool ok = false;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
        sqlite3_busy_timeout(db, CE_BUSY_MS);
        /* A fresh connection has not read the header yet, and a checkpoint
         * on it reports "not WAL" (-1 frames): read the mode first. */
        sqlite3_stmt *st = NULL;
        bool wal = false;
        bool read = sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &st, NULL) == SQLITE_OK &&
                    sqlite3_step(st) == SQLITE_ROW;
        if (read) {
            const char *mode = (const char *)sqlite3_column_text(st, 0);
            wal = mode && strcmp(mode, "wal") == 0;
        }
        sqlite3_finalize(st);
        int frames = -1;
        int copied = -1;
        ok = read && (!wal || (sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                                         &frames, &copied) == SQLITE_OK &&
                               frames >= 0 && frames == copied));
    }
    sqlite3_close(db);
    return ok;
}

/* ── Sweep ──────────────────────────────────────────────────────── */

const char *cbm_cache_expiry_kind_str(cbm_cache_expiry_kind_t kind) {
    switch (kind) {
    case CBM_EXPIRE_ROOT_GONE:
        return "root_gone";
    case CBM_EXPIRE_UNUSED:
        return "unused";
    case CBM_EXPIRE_SLOT:
        return "slot";
    case CBM_EXPIRE_SIDECAR:
        return "sidecar";
    case CBM_EXPIRE_TRASH_PURGE:
        return "trash_purge";
    case CBM_EXPIRE_PENDING:
    default:
        return "pending";
    }
}

typedef struct {
    const cbm_cache_sweep_opts_t *opts;
    cbm_cache_sweep_result_t *res;
} sweep_ctx_t;

static void report(sweep_ctx_t *c, cbm_cache_expiry_kind_t kind, const std::string &name,
                   const std::string &path, int64_t bytes, const std::string &reason, bool removed,
                   const char *skipped) {
    if (removed) {
        cbm_log_info("cache.expire", "kind", cbm_cache_expiry_kind_str(kind), "name",
                     name.c_str(), "path", path.c_str(), "reason", reason.c_str());
    } else if (skipped) {
        cbm_log_info("cache.expire.skip", "kind", cbm_cache_expiry_kind_str(kind), "name",
                     name.c_str(), "why", skipped);
    }
    if (c->opts->report) {
        cbm_cache_expiry_item_t it = {kind,  name.c_str(), path.c_str(), bytes,
                                      reason.c_str(), removed,      skipped};
        c->opts->report(&it, c->opts->report_ud);
    }
}

static void slot_cb(const char *path, int64_t bytes, bool scratch, void *ud) {
    sweep_ctx_t *c = (sweep_ctx_t *)ud;
    const char *base = strrchr(path, '/');
    report(c, scratch ? CBM_EXPIRE_SIDECAR : CBM_EXPIRE_SLOT, base ? base + 1 : path, path, bytes,
           scratch ? "scratch file of an interrupted park or restore" : "parked and not restored",
           !c->opts->dry_run, NULL);
    if (scratch) {
        c->res->sidecars++;
    } else {
        c->res->slots++;
    }
    c->res->bytes += bytes;
}

/* name is the project (session, watch and slot identity, trash entry); stem
 * is the filename without ".db". They differ for a legacy or renamed file. */
typedef struct {
    std::string name;
    std::string stem;
    std::string path;
    usage_row_t row;
    root_state_t state;
} db_entry_t;

static bool refuse(cbm_cache_sweep_result_t *res, const char *why) {
    res->refused = true;
    snprintf(res->refused_reason, sizeof(res->refused_reason), "%s", why);
    cbm_log_warn("cache.expire.refused", "reason", why);
    return false;
}

/* Purge trash entries ("<epoch>-<project>") older than cfg->trash_s. */
static void purge_trash(const char *cdir, const cbm_cache_expiry_cfg_t *cfg, int64_t now,
                        sweep_ctx_t *c) {
    std::string tdir = std::string(cdir) + "/trash";
    cbm_dir_t *d = cbm_opendir(tdir.c_str());
    if (!d) {
        return;
    }
    std::vector<std::string> due;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (!ent->is_dir || ent->name[0] == '.') {
            continue;
        }
        char *end = NULL;
        long long t = strtoll(ent->name, &end, 10);
        if (end && *end == '-' && t > 0 && now - t >= cfg->trash_s) {
            due.push_back(ent->name);
        }
    }
    cbm_closedir(d);
    for (const auto &e : due) {
        std::string p = tdir + "/" + e;
        int64_t bytes = tree_bytes(p.c_str());
        /* cache restore moves an entry back under its project's lease, file
         * by file (database, WAL, slots): a purge in between would leave a
         * partial restore. Purge under the same lease, without waiting; a
         * busy one (a restore, or an index run of the project) keeps the
         * entry for the next sweep. */
        cbm_db_lease_t *lease = NULL;
        if (!c->opts->dry_run) {
            std::string dbp = std::string(cdir) + "/" + e.substr(e.find('-') + 1) + ".db";
            if (cbm_db_lease_try_acquire(dbp.c_str(), &lease) != 0) {
                report(c, CBM_EXPIRE_TRASH_PURGE, e, p, bytes,
                       "in the trash since the previous sweep", false,
                       "its project being restored or indexed");
                c->res->skipped++;
                continue;
            }
            remove_tree(p.c_str());
            cbm_db_lease_release(lease);
        }
        report(c, CBM_EXPIRE_TRASH_PURGE, e, p, bytes, "in the trash since the previous sweep",
               !c->opts->dry_run, NULL);
        c->res->purged++;
        c->res->bytes += bytes;
    }
    if (!c->opts->dry_run) {
        (void)cbm_rmdir(tdir.c_str()); /* only when empty */
    }
}

static bool kept(const cbm_cache_sweep_opts_t *o, const char *project) {
    return (o->keep_project && strcmp(o->keep_project, project) == 0) ||
           (o->keep && o->keep(project, o->keep_ud));
}

/* The project, and its file when that is named otherwise. */
static std::string label(const db_entry_t &e) {
    return e.name == e.stem ? e.name : e.name + " (" + e.stem + ".db)";
}

/* Whether the database's usage row moved past the one the sweep judged.
 * An unreadable row counts as used: the next sweep decides again. */
static bool used_since_scan(const db_entry_t &e) {
    sqlite3 *db = state_open(false);
    if (!db) {
        return true;
    }
    const char *key = cbm_store_meta_key(e.path.c_str());
    sqlite3_stmt *st = NULL;
    bool used = true;
    if (sqlite3_prepare_v2(db,
                           "SELECT last_used, missing_checks FROM cache_usage "
                           "WHERE db_name = ?1;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            used = sqlite3_column_int64(st, 0) > e.row.last_used ||
                   sqlite3_column_int(st, 1) < e.row.missing_checks;
        } else {
            used = rc != SQLITE_DONE; /* no row: nothing recorded a use */
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return used;
}

/* Expire one project database (root_gone or unused) under its lease. */
static void expire_project(const char *cdir, const db_entry_t &e, cbm_cache_expiry_kind_t kind,
                           const std::string &reason, int64_t now, sweep_ctx_t *c) {
    int64_t bytes = path_size(e.path.c_str()) + path_size((e.path + "-wal").c_str());
    cbm_branch_slot_info_t *info = NULL;
    int ni = cbm_branch_slots_info(e.path.c_str(), e.name.c_str(), NULL, &info);
    for (int i = 0; i < ni; i++) {
        bytes += info[i].bytes > 0 ? info[i].bytes : 0;
    }
    cbm_branch_slots_free_info(info, ni);
    std::string entry = std::string(cdir) + "/trash/" + std::to_string((long long)now) + "-" + e.name;
    if (kept(c->opts, e.name.c_str())) {
        report(c, kind, label(e), e.path, bytes, reason, false, "in use by this session");
        c->res->skipped++;
        return;
    }
    if (c->opts->dry_run) {
        report(c, kind, label(e), e.path, bytes, reason, false, NULL);
        c->res->projects++;
        c->res->bytes += bytes;
        return;
    }
    cbm_db_lease_t *lease = NULL;
    if (cbm_db_lease_try_acquire(e.path.c_str(), &lease) != 0) {
        report(c, kind, label(e), e.path, bytes, reason, false, "being indexed");
        c->res->skipped++;
        return;
    }
    /* A renamed file's parked slots are filed under its project, and so are
     * those of the project's own <name>.db, which an index of the project
     * writes under that file's lease, not this one: hold it too. A <name>.db
     * that appeared since the scan owns the slots now; the next scan files
     * this one under its stem. */
    cbm_db_lease_t *canon = NULL;
    if (e.name != e.stem) {
        std::string cpath = std::string(cdir) + "/" + e.name + ".db";
        const char *why = NULL;
        if (cbm_db_lease_try_acquire(cpath.c_str(), &canon) != 0) {
            why = "its project being indexed";
        } else if (cbm_file_exists(cpath.c_str())) {
            why = "its project's own database appeared";
        }
        if (why) {
            cbm_db_lease_release(canon);
            cbm_db_lease_release(lease);
            report(c, kind, label(e), e.path, bytes, reason, false, why);
            c->res->skipped++;
            return;
        }
    }
    /* The lease keeps indexers out, not readers. A reader (a tool call, a
     * hook) holds the file's reader gate shared for as long as it keeps the
     * connection: on POSIX a move would leave it reading the trashed inode
     * while the next call finds no project. Exclusive here means no reader
     * has it open, and a reader that comes now waits until the move is done,
     * then finds the file gone. */
    cbm_db_lease_t *gate = NULL;
    int grc = cbm_db_read_gate_acquire(e.path.c_str(), true, &gate);
    if (grc != 0) {
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false,
               grc == CBM_INDEX_BUSY ? "in use by a reader" : "reader gate unavailable");
        c->res->skipped++;
        return;
    }
    if (!quiesced(e.path.c_str())) {
        cbm_db_lease_release(gate);
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false, "open in a read transaction");
        c->res->skipped++;
        return;
    }
    /* Under the gate, so a reader that opened, recorded its use and closed
     * since the scan is seen here: judge by the row as it is now. */
    if (used_since_scan(e)) {
        cbm_db_lease_release(gate);
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false, "used since the scan");
        c->res->skipped++;
        return;
    }
    int parked = 0;
    int rc = cbm_cache_remove_project(e.path.c_str(), e.name.c_str(), entry.c_str(), &parked);
    cbm_db_lease_release(gate);
    cbm_db_lease_release(canon);
    cbm_db_lease_release(lease);
    if (rc == 0) {
        FILE *f = cbm_fopen((entry + "/EXPIRED").c_str(), "w");
        if (f) {
            fprintf(f, "%s\n%s\nroot=%s\n", cbm_cache_expiry_kind_str(kind), reason.c_str(),
                    e.row.root.c_str());
            fclose(f);
        }
        report(c, kind, label(e), entry, bytes, reason, true, NULL);
        c->res->projects++;
        c->res->bytes += bytes;
    } else if (rc < 0) {
        report(c, kind, label(e), e.path, bytes, reason, false, "could not be moved");
        c->res->skipped++;
    }
}

static bool is_sidecar_name(const char *name, std::string *base) {
    for (const char *suf : CE_SIDECAR_SUFFIXES) {
        if (ends_with(name, suf)) {
            /* <x>.db + suffix after ".db" */
            size_t keep = strlen(name) - strlen(suf) + 3;
            *base = std::string(name, keep);
            return true;
        }
    }
    return false;
}

int cbm_cache_expiry_sweep(const cbm_cache_expiry_cfg_t *cfg, const cbm_cache_sweep_opts_t *opts,
                           cbm_cache_sweep_result_t *res) {
    memset(res, 0, sizeof(*res));
    cbm_cache_sweep_opts_t none = {};
    if (!opts) {
        opts = &none;
    }
    sweep_ctx_t c = {opts, res};
    int64_t now = opts->now > 0 ? opts->now : (int64_t)time(NULL);

    /* Refuse to judge a cache directory that does not look like this user's:
     * no _config.db (a fresh or wrong directory), or HOME gone. */
    const char *cdir = cbm_resolve_cache_dir();
    const char *home = cbm_get_home_dir();
    if (!cdir || !cdir[0] || !cbm_is_dir(cdir)) {
        refuse(res, "cache directory not found");
        return -1;
    }
    std::string cfgdb = std::string(cdir) + "/_config.db";
    if (!cbm_file_exists(cfgdb.c_str())) {
        refuse(res, "cache directory has no _config.db");
        return -1;
    }
    if (!home || !home[0] || !cbm_is_dir(home)) {
        refuse(res, "HOME is not a directory");
        return -1;
    }
    /* One sweep at a time per cache: _config.db's index lease is used by
     * nothing else. */
    cbm_db_lease_t *sweep_lease = NULL;
    if (!opts->dry_run && cbm_db_lease_try_acquire(cfgdb.c_str(), &sweep_lease) != 0) {
        refuse(res, "another sweep is running");
        return -1;
    }
    sqlite3 *db = state_open(true);
    if (!db) {
        cbm_db_lease_release(sweep_lease);
        refuse(res, "cannot open _config.db");
        return -1;
    }

    /* The same cache seen from another HOME or another path (a copied or
     * moved cache): roots are not judged on this sweep, and every streak
     * restarts. */
    std::string env = std::string(home) + "\n" + cdir;
    std::string seen_env = sweep_get(db, "env");
    bool env_changed = !seen_env.empty() && seen_env != env;

    if (cfg->trash_s > 0) {
        purge_trash(cdir, cfg, now, &c);
    }

    /* One scan of the cache directory. */
    std::vector<db_entry_t> dbs;
    std::vector<std::pair<std::string, std::string>> sidecars; /* name, base db */
    cbm_dir_t *d = cbm_opendir(cdir);
    if (d) {
        cbm_dirent_t *ent;
        while ((ent = cbm_readdir(d)) != NULL) {
            if (ent->is_dir) {
                continue;
            }
            std::string base;
            if (ends_with(ent->name, ".db") && strcmp(ent->name, "_config.db") != 0) {
                /* The project is what the file holds, not its filename: a
                 * legacy or renamed file ("old project.db") is served under
                 * its internal name, so it is judged under that name. Only
                 * the name used as a path component (trash entry, slot
                 * directory) has to be a safe one. */
                std::string stem(ent->name, strlen(ent->name) - 3);
                std::string path = std::string(cdir) + "/" + ent->name;
                std::string name = learn_project(path.c_str(), stem);
                if (cbm_validate_project_name(name.c_str())) {
                    dbs.push_back({name, stem, path, {}, ROOT_UNCERTAIN});
                }
            } else if (is_sidecar_name(ent->name, &base) && base != "_config.db") {
                sidecars.emplace_back(ent->name, base);
            }
        }
        cbm_closedir(d);
    }
    /* Two files claiming one project (a stale copy beside the real one) keep
     * their filenames: one trash entry and one slot directory per project. */
    std::unordered_map<std::string, int> claims;
    for (const auto &e : dbs) {
        claims[e.name]++;
    }
    for (auto &e : dbs) {
        if (e.name != e.stem && claims[e.name] > 1) {
            e.name = e.stem;
        }
    }
    /* A stem that is no safe path component cannot stand in: such a copy is
     * not judged. */
    dbs.erase(std::remove_if(dbs.begin(), dbs.end(),
                             [](const db_entry_t &e) {
                                 return !cbm_validate_project_name(e.name.c_str());
                             }),
              dbs.end());

    /* Usage rows; a database seen for the first time starts its clock now,
     * so nothing expires as unused before it has had the full period to be
     * used under this build. Roots are learned once. */
    int known = 0;
    int missing = 0;
    if (!opts->dry_run) {
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    }
    for (auto &e : dbs) {
        const char *key = cbm_store_meta_key(e.path.c_str());
        e.row = usage_get(db, key);
        bool dirty = false;
        if (!e.row.found) {
            e.row.last_used = now;
            dirty = true;
        }
        if (e.row.root.empty()) {
            e.row.root = learn_root(e.path.c_str(), e.name.c_str());
            dirty = dirty || !e.row.root.empty();
        }
        e.state = e.row.root.empty() ? ROOT_UNCERTAIN : root_state(e.row.root);
        known += e.state != ROOT_UNCERTAIN;
        missing += e.state == ROOT_MISSING;
        if (dirty && !opts->dry_run) {
            (void)usage_put(db, key, e.row);
        }
    }
    /* Every judged root missing at once is a moved home or a copied cache,
     * not a cleanup: count nothing. */
    bool mass_missing = known >= 5 && missing == known;
    if (mass_missing) {
        cbm_log_warn("cache.expire.suspect", "reason", "every recorded root is missing");
    }
    bool count_roots = !env_changed && !mass_missing;

    std::vector<std::pair<size_t, cbm_cache_expiry_kind_t>> due;
    std::vector<std::string> reasons;
    for (size_t i = 0; i < dbs.size(); i++) {
        db_entry_t &e = dbs[i];
        const char *key = cbm_store_meta_key(e.path.c_str());
        usage_row_t before = e.row;
        if (e.state == ROOT_PRESENT || env_changed) {
            e.row.missing_since = 0;
            e.row.missing_checks = 0;
        } else if (e.state == ROOT_MISSING && count_roots) {
            if (e.row.missing_checks == 0) {
                e.row.missing_since = now;
            }
            e.row.missing_checks++;
        }
        if (!opts->dry_run && (before.missing_checks != e.row.missing_checks ||
                               before.missing_since != e.row.missing_since)) {
            (void)usage_put(db, key, e.row);
        }
        int64_t idle = now - e.row.last_used;
        bool root_gone = cfg->root_grace_s > 0 && e.state == ROOT_MISSING && count_roots &&
                         e.row.missing_checks >= cfg->root_checks &&
                         now - e.row.missing_since >= cfg->root_grace_s &&
                         idle >= cfg->root_grace_s;
        if (root_gone) {
            due.emplace_back(i, CBM_EXPIRE_ROOT_GONE);
            reasons.push_back("root " + e.row.root + " missing since " +
                              fmt_time(e.row.missing_since) + " (" +
                              std::to_string(e.row.missing_checks) + " sweeps), unused for " +
                              fmt_days(idle) + " days");
        } else if (cfg->unused_s > 0 && idle >= cfg->unused_s) {
            due.emplace_back(i, CBM_EXPIRE_UNUSED);
            reasons.push_back("unused for " + fmt_days(idle) + " days (last used " +
                              fmt_time(e.row.last_used) + ")");
        } else if (e.state == ROOT_MISSING && e.row.missing_checks > 0) {
            report(&c, CBM_EXPIRE_PENDING, label(e), e.path, 0,
                   "root " + e.row.root + " missing since " + fmt_time(e.row.missing_since) +
                       " (" + std::to_string(e.row.missing_checks) + " of " +
                       std::to_string(cfg->root_checks) + " sweeps)",
                   false, NULL);
        }
    }
    if (!opts->dry_run) {
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    db = NULL;

    for (size_t k = 0; k < due.size(); k++) {
        expire_project(cdir, dbs[due[k].first], due[k].second, reasons[k], now, &c);
    }

    /* Parked slots and slot scratch, per project directory under branches/
     * (also of projects whose database is gone), under that project's lease. */
    std::string bdir = std::string(cdir) + "/branches";
    cbm_dir_t *bd = cbm_opendir(bdir.c_str());
    std::vector<std::string> pdirs;
    if (bd) {
        cbm_dirent_t *ent;
        while ((ent = cbm_readdir(bd)) != NULL) {
            if (ent->is_dir && cbm_validate_project_name(ent->name)) {
                pdirs.push_back(ent->name);
            }
        }
        cbm_closedir(bd);
    }
    int64_t slot_before = cfg->slot_s > 0 ? now - cfg->slot_s : 0;
    int64_t scratch_before = cfg->scratch_s > 0 ? now - cfg->scratch_s : 0;
    for (const auto &p : pdirs) {
        std::string dbp = std::string(cdir) + "/" + p + ".db";
        if (kept(opts, p.c_str())) {
            continue;
        }
        cbm_db_lease_t *lease = NULL;
        if (!opts->dry_run && cbm_db_lease_try_acquire(dbp.c_str(), &lease) != 0) {
            continue; /* being indexed: its own switch manages its slots */
        }
        (void)cbm_branch_slots_expire(dbp.c_str(), p.c_str(), slot_before, scratch_before,
                                      opts->dry_run, slot_cb, &c);
        cbm_db_lease_release(lease);
    }

    /* Orphan sidecars: a -wal, -shm, journal or temp file whose database is
     * gone, older than the scratch age, removed under that database's lease
     * (an index run creating it holds the lease). <x>.db.index.lock files
     * stay: the lease's contract is that the lock file is never unlinked. */
    for (const auto &s : sidecars) {
        std::string dbp = std::string(cdir) + "/" + s.second;
        std::string sp = std::string(cdir) + "/" + s.first;
        int64_t mt = path_mtime(sp.c_str());
        if (cbm_file_exists(dbp.c_str()) || mt < 0 || mt >= scratch_before ||
            scratch_before <= 0) {
            continue;
        }
        int64_t bytes = path_size(sp.c_str());
        if (!opts->dry_run) {
            cbm_db_lease_t *lease = NULL;
            if (cbm_db_lease_try_acquire(dbp.c_str(), &lease) != 0) {
                continue;
            }
            bool still = !cbm_file_exists(dbp.c_str());
            if (still) {
                (void)cbm_unlink(sp.c_str());
            }
            cbm_db_lease_release(lease);
            if (!still) {
                continue;
            }
        }
        report(&c, CBM_EXPIRE_SIDECAR, s.first, sp, bytes, "its database " + s.second + " is gone",
               !opts->dry_run, NULL);
        res->sidecars++;
        res->bytes += bytes;
    }

    if (!opts->dry_run) {
        db = state_open(true);
        if (db) {
            sweep_put(db, "env", env);
            sweep_put(db, "last_sweep", std::to_string((long long)now));
            sqlite3_close(db);
        }
    }
    cbm_db_lease_release(sweep_lease);
    cbm_log_info("cache.expire.done", "projects", std::to_string(res->projects).c_str(), "slots",
                 std::to_string(res->slots).c_str(), "sidecars",
                 std::to_string(res->sidecars).c_str(), "mb",
                 std::to_string((long long)(res->bytes >> 20)).c_str(), "dry_run",
                 opts->dry_run ? "true" : "false");
    return 0;
}

bool cbm_cache_expiry_due(const cbm_cache_expiry_cfg_t *cfg, int64_t now) {
    if (!cfg->enabled) {
        return false;
    }
    sqlite3 *db = state_open(true);
    if (!db) {
        return false;
    }
    std::string last = sweep_get(db, "last_sweep");
    sqlite3_close(db);
    return last.empty() || now - strtoll(last.c_str(), NULL, 10) >= cfg->interval_s;
}

/* ── Trash ──────────────────────────────────────────────────────── */

/* Trash entries as (epoch, entry name), newest first. */
static std::vector<std::pair<long long, std::string>> trash_entries(const char *cdir) {
    std::vector<std::pair<long long, std::string>> out;
    std::string tdir = std::string(cdir) + "/trash";
    cbm_dir_t *d = cbm_opendir(tdir.c_str());
    if (!d) {
        return out;
    }
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        char *end = NULL;
        long long t = strtoll(ent->name, &end, 10);
        if (ent->is_dir && end && *end == '-' && t > 0 &&
            !cbm_is_link((tdir + "/" + ent->name).c_str())) {
            out.emplace_back(t, ent->name);
        }
    }
    cbm_closedir(d);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a > b; });
    return out;
}

int cbm_cache_trash_list(void *out) {
    FILE *f = out ? (FILE *)out : stdout;
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir) {
        return 0;
    }
    auto entries = trash_entries(cdir);
    for (const auto &e : entries) {
        std::string p = std::string(cdir) + "/trash/" + e.second;
        fprintf(f, "  %s  (trashed %s, %lld MB)\n", e.second.c_str(), fmt_time(e.first).c_str(),
                (long long)(tree_bytes(p.c_str()) >> 20));
    }
    return (int)entries.size();
}

int cbm_cache_trash_restore(const char *which, char *msg, size_t msg_sz) {
    const char *cdir = cbm_resolve_cache_dir();
    if (!which || !which[0] || !cdir) {
        snprintf(msg, msg_sz, "nothing to restore");
        return -1;
    }
    std::string entry;
    for (const auto &e : trash_entries(cdir)) {
        const char *proj = strchr(e.second.c_str(), '-') + 1;
        if (e.second == which || strcmp(proj, which) == 0) {
            entry = e.second; /* newest first */
            break;
        }
    }
    if (entry.empty()) {
        snprintf(msg, msg_sz, "no trash entry for %s", which);
        return -1;
    }
    std::string project = entry.substr(entry.find('-') + 1);
    std::string edir = std::string(cdir) + "/trash/" + entry;
    std::string dbp = std::string(cdir) + "/" + project + ".db";
    cbm_db_lease_t *lease = NULL;
    if (cbm_db_lease_try_acquire(dbp.c_str(), &lease) != 0) {
        snprintf(msg, msg_sz, "%s is being indexed or purged; retry later", project.c_str());
        return -1;
    }
    int rc = -1;
    if (cbm_file_exists(dbp.c_str())) {
        snprintf(msg, msg_sz, "%s has a database again; delete it first to restore the old one",
                 project.c_str());
    } else {
        std::string src = edir + "/" + project + ".db";
        cbm_remove_db_sidecars(dbp.c_str());
        if (cbm_rename_replace(src.c_str(), dbp.c_str()) != 0) {
            snprintf(msg, msg_sz, "could not move %s back", src.c_str());
        } else {
            std::string wal = src + "-wal";
            if (cbm_file_exists(wal.c_str())) {
                (void)cbm_rename_replace(wal.c_str(), (dbp + "-wal").c_str());
            }
            char sdir[CE_PATH_MAX];
            std::string tb = edir + "/branches";
            bool slots_back = true;
            if (cbm_is_dir(tb.c_str()) &&
                cbm_branch_slots_dir(dbp.c_str(), project.c_str(), NULL, sdir, sizeof(sdir))) {
                std::string parent(sdir, strrchr(sdir, '/') - sdir);
                slots_back = !cbm_is_dir(sdir) && cbm_mkdir_p(parent.c_str(), 0755) &&
                             cbm_rename_replace(tb.c_str(), sdir) == 0;
            }
            /* The clock restarts: it was used just now, by this restore. */
            (void)cbm_cache_usage_set(dbp.c_str(), (int64_t)time(NULL), NULL);
            if (slots_back) {
                remove_tree(edir.c_str());
            }
            snprintf(msg, msg_sz, "restored %s%s", project.c_str(),
                     slots_back ? "" : " (its parked branches stay in the trash)");
            cbm_log_info("cache.restore", "project", project.c_str(), "entry", entry.c_str());
            rc = 0;
        }
    }
    cbm_db_lease_release(lease);
    return rc;
}
