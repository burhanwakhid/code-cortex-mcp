/* cache_expiry.cpp — Remove index databases nobody uses. See cache_expiry.h
 * and docs/CACHE_EXPIRY.md. */
#include "mcp/cache_expiry.h"

#include "cli/cli.h" /* cbm_config_*, CBM_CONFIG_CACHE_EXPIRY* */
#include "foundation/compat.h" /* cbm_usleep */
#include "foundation/compat_fs.h"
#include "foundation/constants.h" /* CBM_INDEX_BUSY */
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "mcp/store_meta.h"
#include "pipeline/branch_slots.h"
#include "store/store.h" /* cbm_store_sqlite_open */

#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* CE_BUSY_MS bounds a wait in the sweep's own reads and probes. A use
 * recorded by a tool call or hook, and the sweep's one short write, wait
 * longer (CE_WRITE_BUSY_MS): a use lost to a busy _config.db lets a sweep
 * trash a database that was just used. */
enum { CE_PATH_MAX = 4096, CE_BUSY_MS = 200, CE_WRITE_BUSY_MS = 2500 };

/* SQLite sidecars of <x>.db, the temp files a dump (<x>.db.tmp) or an
 * artifact import (<x>.db.import_tmp) write before renaming into place, and
 * the sidecars SQLite leaves beside those temp files when the writer dies. */
static const char *const CE_SIDECAR_SUFFIXES[] = {
    ".db-wal",        ".db-shm",        ".db-journal",       ".db.tmp",
    ".db.tmp-wal",    ".db.tmp-shm",    ".db.tmp-journal",   ".db.import_tmp",
    ".db.import_tmp-wal", ".db.import_tmp-shm", ".db.import_tmp-journal",
    /* write_file_atomic adds its own ".tmp" to the import's temp name */
    ".db.import_tmp.tmp",
    /* a failed branch stamp's marker (pipeline.cpp) */
    ".db.stamp-pending"};

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

/* Whether a path is there at all, a dangling link included. */
static bool path_present(const char *path) {
    cbm_stat_t st;
    return cbm_is_link(path) || cbm_stat(path, &st) == 0;
}

/* Delete dir and everything under it. A link, dir itself included, is
 * removed and never followed. True when nothing is left: a refused unlink
 * (a read-only or open file inside) leaves the tree, and the caller must
 * not count it as gone. */
static bool remove_tree(const char *dir) {
    walk_kind_t self = walk_kind(dir);
    if (self != WALK_DIR) {
        if (self == WALK_LINK) {
            remove_link(dir);
        } else {
            (void)cbm_unlink(dir);
        }
        return !path_present(dir);
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
            (void)remove_tree(it.c_str());
        }
    }
    return cbm_rmdir(dir) == 0 || !path_present(dir);
}

/* ── State in _config.db ────────────────────────────────────────── */

/* Whether table `name` exists, from a statement's prepare (a schema read). */
static bool state_has_table(sqlite3 *db, const char *name) {
    std::string sql = std::string("SELECT 1 FROM ") + name + " LIMIT 0;";
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(db, sql.c_str(), -1, &st, NULL) == SQLITE_OK;
    sqlite3_finalize(st);
    return ok;
}

/* read_only: a connection that cannot write at all -- not a row, not a
 * table, not the checkpoint a read-write connection may run as it closes --
 * for the dry run, which promises to write nothing (create_tables is then
 * ignored).
 *
 * busy_ms bounds every wait on this connection, the open's own included: a
 * caller that records a use passes CE_WRITE_BUSY_MS. A writer's open also
 * puts _config.db in WAL mode (once; it persists in the file), as every index
 * database in the cache directory already is: in the default rollback mode a
 * committing writer locks readers out, and a dozen index runs recording their
 * use at once lost rows to the wait. The tables are created only when missing,
 * so an open in WAL mode never needs the write lock just to start. */
static sqlite3 *state_open_once(bool create_tables, bool read_only, int busy_ms, int *err) {
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir || !cdir[0]) {
        return NULL;
    }
    char path[CE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/_config.db", cdir);
    sqlite3 *db = NULL;
    if (read_only) {
        create_tables = false;
    }
    if (cbm_store_sqlite_open(path, &db,
                              read_only ? SQLITE_OPEN_READONLY
                                        : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) !=
        SQLITE_OK) {
        *err = db ? sqlite3_extended_errcode(db) : SQLITE_NOMEM;
        char os_errno[CBM_SZ_32];
        snprintf(os_errno, sizeof(os_errno), "%d", db ? sqlite3_system_errno(db) : 0);
        cbm_log_warn("cache.state.open_failed", "step", "open", "msg",
                     db ? sqlite3_errmsg(db) : "no memory", "errno", os_errno);
        sqlite3_close(db);
        return NULL;
    }
    if (!read_only) {
        /* Keep the -wal between connections. Every use, touch and sweep
         * opens _config.db briefly; without this the last one to close
         * deletes the -wal and the next open, racing it, can fail outright
         * ("unable to open database file"), and that use goes unrecorded. */
        int persist = 1;
        (void)sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);
    }
    sqlite3_busy_timeout(db, busy_ms);
    if (!read_only) {
        sqlite3_stmt *st = NULL;
        bool wal = false;
        if (sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            const char *mode = (const char *)sqlite3_column_text(st, 0);
            wal = mode && strcmp(mode, "wal") == 0;
        }
        sqlite3_finalize(st);
        if (!wal) {
            /* Best effort, and without waiting: the switch needs every other
             * connection gone, so while one is open it can only fail, and a
             * busy wait here spent the whole budget of the write that
             * follows. The next writer's open converts it. */
            sqlite3_busy_timeout(db, 0);
            (void)sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
            sqlite3_busy_timeout(db, busy_ms);
        }
    }
    /* The DDL runs in an IMMEDIATE transaction: it takes the write lock
     * first, under the busy wait. As a bare statement it reads the schema
     * and then asks for the write lock, which SQLite refuses at once,
     * without waiting, while another connection is about to write -- many
     * first opens of a fresh cache at once all failed. */
    if (create_tables &&
        !(state_has_table(db, "cache_usage") && state_has_table(db, "cache_sweep")) &&
        sqlite3_exec(db,
                     "BEGIN IMMEDIATE;"
                     "CREATE TABLE IF NOT EXISTS cache_usage ("
                     "db_name TEXT PRIMARY KEY,"
                     "last_used INTEGER NOT NULL DEFAULT 0,"
                     "root_path TEXT NOT NULL DEFAULT '',"
                     "missing_since INTEGER NOT NULL DEFAULT 0,"
                     "missing_checks INTEGER NOT NULL DEFAULT 0);"
                     "CREATE TABLE IF NOT EXISTS cache_sweep ("
                     "key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                     "COMMIT;",
                     NULL, NULL, NULL) != SQLITE_OK) {
        *err = sqlite3_extended_errcode(db);
        char code[CBM_SZ_32];
        snprintf(code, sizeof(code), "%d", *err);
        cbm_log_warn("cache.state.open_failed", "step", "create_tables", "code", code, "msg",
                     sqlite3_errmsg(db));
        (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/* state_open_once, retried briefly on a transient failure: another
 * connection opening, closing or recovering the WAL at the same moment
 * (SQLITE_CANTOPEN, SQLITE_BUSY, SQLITE_PROTOCOL). A real failure -- a
 * read-only directory, a full disk -- is not retried. The retries stay
 * within busy_ms. */
static sqlite3 *state_open(bool create_tables, bool read_only = false, int busy_ms = CE_BUSY_MS) {
    enum { STEP_MS = 25 };
    for (int waited = 0;; waited += STEP_MS) {
        int err = SQLITE_OK;
        sqlite3 *db = state_open_once(create_tables, read_only, busy_ms, &err);
        int primary = err & 0xff;
        bool transient =
            primary == SQLITE_CANTOPEN || primary == SQLITE_BUSY || primary == SQLITE_PROTOCOL;
        if (db || !transient || waited + STEP_MS > busy_ms) {
            return db;
        }
        cbm_usleep(STEP_MS * 1000);
    }
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

/* A setting from _config.db's config table through a read-only connection:
 * NULL when the file, the table or the key is absent. */
static std::string cfg_read_value(sqlite3 *db, const char *key, bool *found) {
    *found = false;
    std::string v;
    sqlite3_stmt *st = NULL;
    if (db && sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key = ?1;", -1, &st, NULL) ==
                  SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
            v = (const char *)sqlite3_column_text(st, 0);
            *found = true;
        }
    }
    sqlite3_finalize(st);
    return v;
}

/* cbm_config_get_bool / _int's parsing, on a value read above. */
static bool cfg_parse_bool(const std::string &v, bool found, bool def) {
    if (!found) {
        return def;
    }
    if (v == "true" || v == "1" || v == "on") {
        return true;
    }
    if (v == "false" || v == "0" || v == "off") {
        return false;
    }
    return def;
}

static int cfg_parse_int(const std::string &v, bool found, int def) {
    if (!found || v.empty()) {
        return def;
    }
    char *end = NULL;
    errno = 0;
    long long n = strtoll(v.c_str(), &end, 10);
    /* Out of int's range is the default, never a wrapped value: 4294967297
     * days narrowed to 1 and expired everything after a day. */
    if (!end || *end != '\0' || errno == ERANGE || n < INT_MIN || n > INT_MAX) {
        return def;
    }
    return (int)n;
}

static void cfg_load_read_only(cbm_cache_expiry_cfg_t *out) {
    const char *dir = cbm_resolve_cache_dir();
    if (!dir || !dir[0]) {
        return;
    }
    std::string path = std::string(dir) + "/_config.db";
    if (!cbm_file_exists(path.c_str())) {
        return;
    }
    sqlite3 *db = NULL;
    if (cbm_store_sqlite_open_inspect(path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, CE_WRITE_BUSY_MS);
    bool f = false;
    std::string v = cfg_read_value(db, CBM_CONFIG_CACHE_EXPIRY, &f);
    out->enabled = cfg_parse_bool(v, f, true);
    v = cfg_read_value(db, CBM_CONFIG_CACHE_EXPIRY_UNUSED_DAYS, &f);
    out->unused_s = (int64_t)cfg_parse_int(v, f, CBM_CACHE_EXPIRY_UNUSED_DAYS) * CBM_CACHE_DAY_S;
    v = cfg_read_value(db, CBM_CONFIG_CACHE_EXPIRY_ROOT_GRACE_DAYS, &f);
    out->root_grace_s =
        (int64_t)cfg_parse_int(v, f, CBM_CACHE_EXPIRY_ROOT_GRACE_DAYS) * CBM_CACHE_DAY_S;
    v = cfg_read_value(db, CBM_CONFIG_CACHE_EXPIRY_SLOT_DAYS, &f);
    out->slot_s = (int64_t)cfg_parse_int(v, f, CBM_CACHE_EXPIRY_SLOT_DAYS) * CBM_CACHE_DAY_S;
    sqlite3_close(db);
}

void cbm_cache_expiry_cfg_load(cbm_cache_expiry_cfg_t *out, struct cbm_config *cfg) {
    out->enabled = true;
    out->unused_s = (int64_t)CBM_CACHE_EXPIRY_UNUSED_DAYS * CBM_CACHE_DAY_S;
    out->root_grace_s = (int64_t)CBM_CACHE_EXPIRY_ROOT_GRACE_DAYS * CBM_CACHE_DAY_S;
    out->root_checks = CBM_CACHE_EXPIRY_ROOT_CHECKS;
    out->slot_s = (int64_t)CBM_CACHE_EXPIRY_SLOT_DAYS * CBM_CACHE_DAY_S;
    out->trash_s = CBM_CACHE_EXPIRY_TRASH_S;
    out->scratch_s = CBM_CACHE_EXPIRY_SCRATCH_S;
    out->interval_s = CBM_CACHE_EXPIRY_INTERVAL_S;
    if (!cfg) {
        /* The settings are read through a read-only connection, never
         * cbm_config_open: that one creates the cache directory, _config.db
         * and its config table and switches the journal mode, and a preview
         * (`cache expire --dry-run`) must write nothing. */
        cfg_load_read_only(out);
    } else {
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
    const char *env = getenv("CBM_CACHE_EXPIRY");
    if (env && strcmp(env, "0") == 0) {
        out->enabled = false;
    }
}

/* ── Usage ──────────────────────────────────────────────────────── */

bool cbm_cache_usage_set(const char *db_path, int64_t last_used, const char *root_path) {
    const char *key = cbm_store_meta_key(db_path);
    sqlite3 *db = key[0] ? state_open(true, false, CE_WRITE_BUSY_MS) : NULL;
    if (!db) {
        if (key[0]) {
            cbm_log_warn("cache.usage.not_recorded", "db", key, "reason", "config_unavailable");
        }
        return false;
    }
    /* Read-modify-write under the write lock: a query touch (no root) in
     * another process reading the row before this and writing after it
     * would put the old root and missing streak back. */
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK;
    if (ok) {
        usage_row_t r = usage_get(db, key);
        if (last_used > 0) {
            r.last_used = last_used;
        }
        if (root_path) {
            r.root = root_path;
        }
        r.missing_since = 0;
        r.missing_checks = 0;
        ok = usage_put(db, key, r) &&
             sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
        if (!ok) {
            (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        }
    }
    if (!ok) {
        cbm_log_warn("cache.usage.not_recorded", "db", key, "reason", sqlite3_errmsg(db));
    }
    sqlite3_close(db);
    return ok;
}

/* Per database, when this process last recorded its use. */
static std::mutex g_touch_mu;
static std::unordered_map<std::string, int64_t> g_touch_seen;

void cbm_cache_usage_forget(const char *db_path) {
    const char *key = cbm_store_meta_key(db_path);
    if (!key[0]) {
        return;
    }
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    cbm_store_meta_forget(meta, db_path);
    cbm_store_meta_close(meta);
    sqlite3 *db = state_open(false, false, CE_WRITE_BUSY_MS);
    usage_forget(db, key);
    sqlite3_close(db);
    std::lock_guard<std::mutex> g(g_touch_mu);
    g_touch_seen.erase(key);
}

void cbm_cache_usage_touch(const char *db_path, const char *root_path) {
    const char *key = cbm_store_meta_key(db_path);
    if (!key[0]) {
        return;
    }
    int64_t now = (int64_t)time(NULL);
    std::mutex &mu = g_touch_mu;
    std::unordered_map<std::string, int64_t> &seen = g_touch_seen;
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = seen.find(key);
        if (!root_path && it != seen.end() && now - it->second < CBM_CACHE_USAGE_RESOLUTION_S) {
            return;
        }
    }
    /* Read first: a hook process lands here once per database it opens, and
     * a read never takes _config.db's write lock. */
    sqlite3 *db = state_open(true, false, CE_WRITE_BUSY_MS);
    if (!db) {
        cbm_log_warn("cache.usage.not_recorded", "db", key, "reason", "config_unavailable");
        return;
    }
    usage_row_t r = usage_get(db, key);
    bool fresh = r.found && now - r.last_used < CBM_CACHE_USAGE_RESOLUTION_S;
    bool root_known = !root_path || (r.root == root_path && r.missing_checks == 0);
    bool ok = true;
    if (!fresh || !root_known) {
        /* The write re-reads the row under the write lock: between the read
         * above and this write an index run may have recorded a new root and
         * cleared the missing streak, and a stale copy written over it would
         * undo that -- after the grace period the sweep would trash an index
         * a live checkout owns. */
        ok = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK;
        if (ok) {
            r = usage_get(db, key);
            r.last_used = now;
            if (root_path) {
                r.root = root_path;
                r.missing_since = 0;
                r.missing_checks = 0;
            }
            ok = usage_put(db, key, r) &&
                 sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
            if (!ok) {
                (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            }
        }
        if (!ok) {
            cbm_log_warn("cache.usage.not_recorded", "db", key, "reason", sqlite3_errmsg(db));
        }
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

#ifdef CBM_ENABLE_TEST_SEAMS
static std::string g_test_mount_parent;
static std::string g_test_mount_table;

void cbm_cache_expiry_test_mount_table(const char *path) {
    g_test_mount_table = path ? path : "";
}

void cbm_cache_expiry_test_mount_parent(const char *dir) {
    g_test_mount_parent = dir ? dir : "";
    std::replace(g_test_mount_parent.begin(), g_test_mount_parent.end(), '\\', '/');
}
#endif

/* Directories under which a missing path may only be an unmounted volume. */
static bool is_mount_parent(const std::string &dir) {
#ifdef CBM_ENABLE_TEST_SEAMS
    if (!g_test_mount_parent.empty() && dir == g_test_mount_parent) {
        return true;
    }
#endif
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
    /* A UNC share root (//server/share, separators folded): a missing share
     * is an unreachable server, not a removed checkout. */
    if (dir.size() > 2 && dir[0] == '/' && dir[1] == '/' &&
        std::count(dir.begin(), dir.end(), '/') <= 3) {
        return true;
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
/* A mount-point field of fstab or the mount table, its octal escapes
 * decoded: a space is written "\\040", a tab "\\011", a backslash "\\134".
 * Compared undecoded, "/srv/My Disk" never matched its "/srv/My\\040Disk"
 * entry, and an unmounted checkout there counted as deleted. */
static std::string mount_field_decode(const char *field) {
    std::string out;
    for (const char *p = field; *p; p++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' &&
            p[3] >= '0' && p[3] <= '7') {
            out += (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else {
            out += *p;
        }
    }
    return out;
}

/* Whether an existing directory looks like a mount point whose volume is
 * not mounted right now: it is named as a mount point in /etc/fstab or the
 * mount table, or it is empty (a mount point directory is) AND sits directly
 * in a mount parent (/mnt/data, /media/me/usb, /data under "/"). A checkout
 * below it is unavailable, not deleted. An empty directory anywhere else is
 * what a removal leaves behind: `git worktree remove` of repo/.worktrees/x
 * leaves repo/.worktrees empty, and that worktree is deleted. */
static bool looks_like_mount_point(const std::string &dir) {
    size_t cut = dir.find_last_of('/');
    std::string parent = cut == std::string::npos ? "" : cut == 0 ? "/" : dir.substr(0, cut);
#ifdef _WIN32
    if (parent.size() == 2 && parent[1] == ':') {
        parent += '/'; /* C:/data: its parent is the drive root */
    }
#endif
    cbm_dir_t *d = !parent.empty() && is_mount_parent(parent) ? cbm_opendir(dir.c_str()) : NULL;
    if (d) {
        bool empty = true;
        cbm_dirent_t *ent;
        while ((ent = cbm_readdir(d)) != NULL) {
            if (strcmp(ent->name, ".") != 0 && strcmp(ent->name, "..") != 0) {
                empty = false;
                break;
            }
        }
        cbm_closedir(d);
        if (empty) {
            return true;
        }
    }
#ifndef _WIN32
    const char *tables[] = {"/etc/fstab", "/proc/self/mounts", NULL};
#ifdef CBM_ENABLE_TEST_SEAMS
    if (!g_test_mount_table.empty()) {
        tables[2] = g_test_mount_table.c_str();
    }
#endif
    for (const char *table : tables) {
        if (!table) {
            continue;
        }
        FILE *f = fopen(table, "r");
        if (!f) {
            continue;
        }
        char line[CE_PATH_MAX];
        bool found = false;
        while (!found && fgets(line, sizeof(line), f)) {
            if (line[0] == '#') {
                continue;
            }
            /* "<device> <mount point> <type> ..." in both files */
            char *save = NULL;
            char *dev = strtok_r(line, " \t\n", &save);
            char *mnt = dev ? strtok_r(NULL, " \t\n", &save) : NULL;
            found = mnt && dir == mount_field_decode(mnt);
        }
        fclose(f);
        if (found) {
            return true;
        }
    }
#endif
    return false;
}

static root_state_t root_state(const std::string &root) {
    bool absolute = !root.empty() && root[0] == '/';
#ifdef _WIN32
    /* A drive path, or a UNC share (\\server\share\repo) on either separator. */
    absolute = absolute || (root.size() > 2 && root[1] == ':') ||
               (root.size() > 2 && (root[0] == '\\' || root[0] == '/') &&
                (root[1] == '\\' || root[1] == '/'));
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
    /* A link that is there but leads nowhere -- a symlink or junction to a
     * volume that is not mounted -- says nothing about whether the checkout
     * behind it still exists: uncertain, here and on the walk up. cbm_stat
     * follows links, so a dangling one reads as "no such file" and the walk
     * would go on to a real ancestor and call the root deleted. */
    if (cbm_is_link(root.c_str())) {
        return ROOT_UNCERTAIN;
    }
    std::string dir = root;
#ifdef _WIN32
    /* A stored Windows root is written with backslashes; the walk up splits
     * on '/' and the Windows file calls take either, so fold them first.
     * Without this a deleted `C:\Users\me\repo` found no separator, was
     * classed uncertain, and never started its missing streak. */
    for (char &c : dir) {
        if (c == '\\') {
            c = '/';
        }
    }
#endif
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    for (;;) {
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos) {
            return ROOT_UNCERTAIN;
        }
        dir = slash == 0 ? "/" : dir.substr(0, slash);
#ifdef _WIN32
        if (dir.size() == 2 && dir[1] == ':') {
            dir += '/'; /* the drive root itself, not the drive's current dir */
        }
#endif
        if (cbm_stat(dir.c_str(), &st) == 0) {
            /* Windows stats a junction itself, not its target: a dangling
             * one "exists". An ancestor that is a link must lead somewhere
             * that can be listed, else it is the unmounted case above. */
            if (cbm_is_link(dir.c_str())) {
                cbm_dir_t *probe = cbm_opendir(dir.c_str());
                if (!probe) {
                    return ROOT_UNCERTAIN;
                }
                cbm_closedir(probe);
            }
            /* The nearest existing ancestor: a mount parent, or itself a
             * mount point with nothing mounted (/mnt/data of an unmounted
             * /mnt/data/repo), means unavailable, not deleted. */
            return is_mount_parent(dir) || looks_like_mount_point(dir) ? ROOT_UNCERTAIN
                                                                        : ROOT_MISSING;
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            return ROOT_UNCERTAIN;
        }
        if (cbm_is_link(dir.c_str())) {
            return ROOT_UNCERTAIN; /* a dangling link on the way up */
        }
        if (dir == "/" || (dir.size() == 3 && dir[1] == ':')) {
            return ROOT_UNCERTAIN; /* the root itself is unreachable */
        }
    }
}

/* The memo, for lookups. A dry run opens it read-only: the ordinary open
 * may drop a v1 table and create the current one in _config.db. */
static cbm_store_meta_db_t *meta_open(bool dry_run) {
    return dry_run ? cbm_store_meta_open_readonly() : cbm_store_meta_open();
}

/* The root a database records, without the store layer: the memo row at the
 * file's current generation, else one read of its projects table. */
static std::string learn_root(const char *path, const char *project, bool dry_run) {
    cbm_file_gen_t gen;
    if (cbm_file_generation(path, &gen)) {
        cbm_store_meta_db_t *meta = meta_open(dry_run);
        cbm_store_meta_row_t row;
        /* A memo root that fills its column may be truncated (the memo keeps
         * CBM_STORE_META_PATH_MAX bytes): such a root would read as missing
         * and start a streak against a live checkout, so the database's own
         * projects table is read instead. */
        bool hit = cbm_store_meta_get(meta, path, &gen, &row) && row.root_path[0] &&
                   strlen(row.root_path) < CBM_STORE_META_PATH_MAX - 1;
        cbm_store_meta_close(meta);
        if (hit) {
            return row.root_path;
        }
    }
    std::string root;
    sqlite3 *db = NULL;
    if ((dry_run ? cbm_store_sqlite_open_inspect(path, &db)
                 : cbm_store_sqlite_open(path, &db, SQLITE_OPEN_READONLY)) == SQLITE_OK) {
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
 * or renamed file is served under that internal name. *is_index is false
 * for a file whose `projects` table cannot even be queried -- not an SQLite
 * database, or one some other tool keeps in the cache directory: no index
 * of ours, so the sweep never judges it by its filename (nor removes it). A
 * memo row names only an index. */
static std::string learn_project(const char *path, const std::string &stem, bool dry_run,
                                 bool *is_index) {
    *is_index = true;
    cbm_file_gen_t gen;
    if (cbm_file_generation(path, &gen)) {
        cbm_store_meta_db_t *meta = meta_open(dry_run);
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
    *is_index = false;
    if ((dry_run ? cbm_store_sqlite_open_inspect(path, &db)
                 : cbm_store_sqlite_open(path, &db, SQLITE_OPEN_READONLY)) == SQLITE_OK) {
        sqlite3_busy_timeout(db, CE_BUSY_MS);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT name FROM projects WHERE instr(name, '::') = 0 LIMIT 2;",
                               -1, &st, NULL) == SQLITE_OK) {
            *is_index = true;
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

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*g_scan_test_cb)(void *) = nullptr;
static void *g_scan_test_data = nullptr;

void cbm_cache_expiry_test_scan_hook(void (*callback)(void *data), void *data) {
    g_scan_test_cb = callback;
    g_scan_test_data = data;
}
#endif

/* ── Removal ────────────────────────────────────────────────────── */

static bool trash_dir_usable(const std::string &tdir);

static int remove_project(const char *db_path, const char *project, const char *root,
                          const char *trash_entry, int *parked);

int cbm_cache_remove_project(const char *db_path, const char *project, const char *trash_entry,
                             int *parked) {
    return remove_project(db_path, project, NULL, trash_entry, parked);
}

int cbm_cache_remove_project_root(const char *db_path, const char *project, const char *root,
                                  int *parked) {
    return remove_project(db_path, project, root && root[0] ? root : NULL, NULL, parked);
}

/* root NULL: every root's parked slots; else only that checkout's (and never
 * moved to the trash: trash_entry is NULL then). */
static int remove_project(const char *db_path, const char *project, const char *root,
                          const char *trash_entry, int *parked) {
    if (parked) {
        *parked = 0;
    }
    char wal[CE_PATH_MAX];
    char shm[CE_PATH_MAX];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    bool exists = cbm_file_exists(db_path);
    int rc = exists ? 0 : 1;
    /* Nothing is moved through a link standing in for the trash directory
     * or the entry: the file would land outside the cache. */
    bool linked = false;
    if (trash_entry) {
        std::string tdir(trash_entry);
        size_t cut = tdir.find_last_of("/\\");
        linked = cbm_is_link(trash_entry) ||
                 (cut != std::string::npos && cut > 0 && !trash_dir_usable(tdir.substr(0, cut)));
    }
    if (exists && trash_entry) {
        char dst[CE_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%s.db", trash_entry, project);
        if (linked || !cbm_mkdir_p(trash_entry, 0755) || cbm_rename_replace(db_path, dst) != 0) {
            rc = -1;
        } else if (cbm_file_exists(wal)) {
            /* The sweep only moves a quiesced database (every frame in the
             * main file), so a WAL that will not move is deleted instead;
             * one that will not go either is not left beside a missing
             * database (a recreated one would read its stale frames): the
             * main file goes back and the removal fails. */
            char wdst[CE_PATH_MAX];
            snprintf(wdst, sizeof(wdst), "%s/%s.db-wal", trash_entry, project);
            if (cbm_rename_replace(wal, wdst) != 0 && cbm_unlink(wal) != 0 &&
                cbm_file_exists(wal)) {
                cbm_log_warn("cache.remove.wal_stuck", "path", wal);
                (void)cbm_rename_replace(dst, db_path); /* roll the main file back */
                rc = -1;
            }
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
    cbm_cache_usage_forget(db_path);
    char **slots = NULL;
    int n = cbm_branch_slots_list(db_path, project, root, &slots);
    cbm_branch_slots_free_list(slots, n);
    char sdir[CE_PATH_MAX];
    if (trash_entry && !root && n > 0 &&
        cbm_branch_slots_dir(db_path, project, NULL, sdir, sizeof(sdir))) {
        char dst[CE_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/branches", trash_entry);
        if (!linked && cbm_mkdir_p(trash_entry, 0755) && cbm_rename_replace(sdir, dst) == 0) {
            if (parked) {
                *parked = n;
            }
            return rc;
        }
    }
    int removed = root ? cbm_branch_slots_delete_root(db_path, project, root)
                       : cbm_branch_slots_delete_all(db_path, project);
    if (parked) {
        *parked = removed;
    }
    /* A slot that refused to go (a read-only directory, a sharing violation)
     * would restore graph data the caller was told is gone: reported, not
     * hidden behind the main file's success. */
    slots = NULL;
    int left = cbm_branch_slots_list(db_path, project, root, &slots);
    cbm_branch_slots_free_list(slots, left);
    if (left > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", left);
        cbm_log_warn("cache.remove.slots_left", "project", project, "slots", buf);
        return 2;
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
    if (cbm_store_sqlite_open(path, &db, SQLITE_OPEN_READWRITE) == SQLITE_OK) {
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
    /* Projects a dry run reported as removed: their parked slots are in
     * the project's bytes already, as the real sweep moves them with it,
     * so the slot pass does not report them again. */
    std::unordered_set<std::string> previewed;
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
    usage_row_t seen; /* the row as the scan read it */
    bool moved;       /* the row changed after the scan, or could not be written */
} db_entry_t;

static bool refuse(cbm_cache_sweep_result_t *res, const char *why) {
    res->refused = true;
    snprintf(res->refused_reason, sizeof(res->refused_reason), "%s", why);
    cbm_log_warn("cache.expire.refused", "reason", why);
    return false;
}

/* Whether <cache>/trash is a real directory, or absent. A link in its place
 * (a symlink, a junction) is never followed: entries found through it would
 * be outside the cache, and removed from there. */
static bool trash_dir_usable(const std::string &tdir) {
    if (walk_kind(tdir.c_str()) != WALK_LINK) {
        return true;
    }
    cbm_log_warn("cache.expire.refused", "reason", "trash directory is a link", "path",
                 tdir.c_str());
    return false;
}

/* Purge trash entries ("<epoch>-<project>") older than cfg->trash_s. */
static void purge_trash(const char *cdir, const cbm_cache_expiry_cfg_t *cfg, int64_t now,
                        sweep_ctx_t *c) {
    std::string tdir = std::string(cdir) + "/trash";
    if (!trash_dir_usable(tdir)) {
        return;
    }
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
        std::string dbp = std::string(cdir) + "/" + e.substr(e.find('-') + 1) + ".db";
        if ((c->opts->dry_run ? cbm_db_lease_probe(dbp.c_str())
                              : cbm_db_lease_try_acquire(dbp.c_str(), &lease)) != 0) {
            report(c, CBM_EXPIRE_TRASH_PURGE, e, p, bytes, "in the trash since the previous sweep",
                   false, "its project being restored or indexed");
            c->res->skipped++;
            continue;
        }
        if (!c->opts->dry_run) {
            bool gone = remove_tree(p.c_str());
            cbm_db_lease_release(lease);
            if (!gone) {
                /* Something inside refused to go (a read-only or open
                 * file): the entry stays for the next sweep, and is not
                 * counted as reclaimed. */
                cbm_log_warn("cache.expire.skipped", "kind", "trash", "path", p.c_str(), "reason",
                             "remove_failed");
                report(c, CBM_EXPIRE_TRASH_PURGE, e, p, bytes,
                       "in the trash since the previous sweep", false,
                       "a file in it could not be removed");
                c->res->skipped++;
                continue;
            }
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
 * An unreadable row counts as used: the next sweep decides again. A real
 * sweep wrote its updated row (e.row) before judging; a dry run wrote
 * nothing, so the stored row is still the one the scan read (e.seen), and
 * comparing with e.row would read the streak's in-memory increment as a
 * streak that went back -- every root_gone verdict previewed as kept. */
static bool used_since_scan(const db_entry_t &e, bool dry_run) {
    const usage_row_t &judged = dry_run ? e.seen : e.row;
    sqlite3 *db = state_open(false, dry_run);
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
            used = sqlite3_column_int64(st, 0) > judged.last_used ||
                   sqlite3_column_int(st, 1) < judged.missing_checks;
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
    /* A preview runs every check below as the sweep would -- the lease,
     * the reader gate, quiescence, a use since the scan, the root -- and
     * only then says "would be removed" instead of moving the file. */
    /* A preview probes the locks (nothing created, nothing held) and skips
     * the quiescence check, which is a checkpoint: it writes. */
    const bool dry = c->opts->dry_run;
    cbm_db_lease_t *lease = NULL;
    if ((dry ? cbm_db_lease_probe(e.path.c_str())
             : cbm_db_lease_try_acquire(e.path.c_str(), &lease)) != 0) {
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
        if ((dry ? cbm_db_lease_probe(cpath.c_str())
                 : cbm_db_lease_try_acquire(cpath.c_str(), &canon)) != 0) {
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
    int grc = dry ? cbm_db_read_gate_probe(e.path.c_str())
                  : cbm_db_read_gate_acquire(e.path.c_str(), true, &gate);
    if (grc != 0) {
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false,
               grc == CBM_INDEX_BUSY ? "in use by a reader" : "reader gate unavailable");
        c->res->skipped++;
        return;
    }
    if (!dry && !quiesced(e.path.c_str())) {
        cbm_db_lease_release(gate);
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false, "open in a read transaction");
        c->res->skipped++;
        return;
    }
    /* Under the gate, so a reader that opened, recorded its use and closed
     * since the scan is seen here: judge by the row as it is now. */
    if (used_since_scan(e, dry)) {
        cbm_db_lease_release(gate);
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false, "used since the scan");
        c->res->skipped++;
        return;
    }
    /* The root was judged missing at the scan; a volume remounted or a
     * checkout restored since then is here again. One stat, under the gate. */
    if (kind == CBM_EXPIRE_ROOT_GONE && root_state(e.row.root) != ROOT_MISSING) {
        cbm_db_lease_release(gate);
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false, "root present again");
        c->res->skipped++;
        return;
    }
    if (c->opts->dry_run) {
        cbm_db_lease_release(gate);
        cbm_db_lease_release(canon);
        cbm_db_lease_release(lease);
        report(c, kind, label(e), e.path, bytes, reason, false, NULL);
        c->res->projects++;
        c->res->bytes += bytes;
        c->previewed.insert(e.name);
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
    } else if (rc == 2) {
        /* The database went; some parked slot could not be removed and
         * waits for the slot expiry of a later sweep. Reported as removed
         * (it is), the slots' refusal carried in the reason, and only what
         * actually went counted: the slots still on disk are measured
         * again and left out of the reclaimed bytes. */
        int64_t left = 0;
        info = NULL;
        ni = cbm_branch_slots_info(e.path.c_str(), e.name.c_str(), NULL, &info);
        for (int i = 0; i < ni; i++) {
            left += info[i].bytes > 0 ? info[i].bytes : 0;
        }
        cbm_branch_slots_free_info(info, ni);
        int64_t gone = bytes > left ? bytes - left : 0;
        report(c, kind, label(e), entry, gone,
               reason + "; some parked branches could not be removed and stay for a later sweep",
               true, NULL);
        c->res->projects++;
        c->res->bytes += gone;
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
    sweep_ctx_t c = {opts, res, {}};
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
    /* A preview creates no state tables either: rows it cannot read are
     * "never seen", which is what a real sweep would record first. */
    sqlite3 *db = state_open(true, opts->dry_run);
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
                /* A <name>.db that is a link (a symlink, a reparse point)
                 * points outside the cache: never opened, judged or moved
                 * -- the sweep's no-follow boundary holds for files too. */
                if (cbm_is_link(path.c_str())) {
                    cbm_log_warn("cache.expire.skipped", "kind", "database", "path",
                                 path.c_str(), "reason", "is_link");
                    continue;
                }
                bool is_index = true;
                std::string name = learn_project(path.c_str(), stem, opts->dry_run, &is_index);
                if (!is_index) {
                    cbm_log_info("cache.expire.skipped", "kind", "database", "path",
                                 path.c_str(), "reason", "not_an_index");
                    continue;
                }
                if (cbm_validate_project_name(name.c_str())) {
                    dbs.push_back({name, stem, path, {}, ROOT_UNCERTAIN, {}, false});
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
     * used under this build. Roots are learned once. Learning a root opens
     * the database and judging it stats the disk (a network mount can take
     * seconds), so the scan holds no transaction: a tool call or hook
     * recording a use meanwhile must not lose its write to a busy
     * _config.db. Rows are written afterwards, in one short transaction. */
    int known = 0;
    int missing = 0;
    for (auto &e : dbs) {
        const char *key = cbm_store_meta_key(e.path.c_str());
        e.row = usage_get(db, key);
        e.seen = e.row;
        if (!e.row.found) {
            e.row.last_used = now;
        }
        if (e.row.root.empty()) {
            e.row.root = learn_root(e.path.c_str(), e.name.c_str(), opts->dry_run);
        }
        e.state = e.row.root.empty() ? ROOT_UNCERTAIN : root_state(e.row.root);
        known += e.state != ROOT_UNCERTAIN;
        missing += e.state == ROOT_MISSING;
    }
#ifdef CBM_ENABLE_TEST_SEAMS
    if (g_scan_test_cb) {
        g_scan_test_cb(g_scan_test_data);
    }
#endif
    /* Every judged root missing at once is a moved home or a copied cache,
     * not a cleanup: count nothing. */
    bool mass_missing = known >= 5 && missing == known;
    if (mass_missing) {
        cbm_log_warn("cache.expire.suspect", "reason", "every recorded root is missing");
    }
    bool count_roots = !env_changed && !mass_missing;

    for (auto &e : dbs) {
        if (e.state == ROOT_PRESENT || env_changed) {
            e.row.missing_since = 0;
            e.row.missing_checks = 0;
        } else if (e.state == ROOT_MISSING && count_roots) {
            if (e.row.missing_checks == 0) {
                e.row.missing_since = now;
            }
            e.row.missing_checks++;
        }
    }
    /* Write the rows that changed. A row that moved since the scan read it
     * recorded a use (or a new root) meanwhile: it keeps that write and is
     * not judged on this sweep. */
    if (!opts->dry_run) {
        sqlite3_busy_timeout(db, CE_WRITE_BUSY_MS);
        bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK;
        for (auto &e : dbs) {
            if (!ok) {
                e.moved = true; /* nothing recorded: judge nothing */
                continue;
            }
            const usage_row_t &a = e.seen;
            const usage_row_t &b = e.row;
            bool changed = !a.found || a.last_used != b.last_used || a.root != b.root ||
                           a.missing_since != b.missing_since ||
                           a.missing_checks != b.missing_checks;
            if (!changed) {
                continue;
            }
            usage_row_t cur = usage_get(db, cbm_store_meta_key(e.path.c_str()));
            if (cur.found != a.found || cur.last_used != a.last_used || cur.root != a.root ||
                cur.missing_since != a.missing_since || cur.missing_checks != a.missing_checks) {
                e.moved = true;
                continue;
            }
            ok = usage_put(db, cbm_store_meta_key(e.path.c_str()), e.row);
            e.moved = !ok;
        }
        if (ok) {
            ok = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
        }
        if (!ok) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            for (auto &e : dbs) {
                e.moved = true;
            }
        }
    }
    sqlite3_close(db);
    db = NULL;

    std::vector<std::pair<size_t, cbm_cache_expiry_kind_t>> due;
    std::vector<std::string> reasons;
    for (size_t i = 0; i < dbs.size(); i++) {
        db_entry_t &e = dbs[i];
        int64_t idle = now - e.row.last_used;
        bool root_gone = cfg->root_grace_s > 0 && e.state == ROOT_MISSING && count_roots &&
                         e.row.missing_checks >= cfg->root_checks &&
                         now - e.row.missing_since >= cfg->root_grace_s &&
                         idle >= cfg->root_grace_s;
        cbm_cache_expiry_kind_t kind = CBM_EXPIRE_PENDING;
        std::string reason;
        if (root_gone) {
            kind = CBM_EXPIRE_ROOT_GONE;
            reason = "root " + e.row.root + " missing since " + fmt_time(e.row.missing_since) +
                     " (" + std::to_string(e.row.missing_checks) + " sweeps), unused for " +
                     fmt_days(idle) + " days";
        } else if (cfg->unused_s > 0 && idle >= cfg->unused_s) {
            kind = CBM_EXPIRE_UNUSED;
            reason = "unused for " + fmt_days(idle) + " days (last used " +
                     fmt_time(e.row.last_used) + ")";
        } else if (e.state == ROOT_MISSING && e.row.missing_checks > 0) {
            report(&c, CBM_EXPIRE_PENDING, label(e), e.path, 0,
                   "root " + e.row.root + " missing since " + fmt_time(e.row.missing_since) +
                       " (" + std::to_string(e.row.missing_checks) + " of " +
                       std::to_string(cfg->root_checks) + " sweeps)",
                   false, NULL);
            continue;
        } else {
            continue;
        }
        if (e.moved) {
            report(&c, kind, label(e), e.path, 0, reason, false, "used since the scan");
            res->skipped++;
            continue;
        }
        due.emplace_back(i, kind);
        reasons.push_back(reason);
    }

    for (size_t k = 0; k < due.size(); k++) {
        expire_project(cdir, dbs[due[k].first], due[k].second, reasons[k], now, &c);
    }

    /* Parked slots and slot scratch, per project directory under branches/
     * (also of projects whose database is gone), under that project's lease. */
    std::string bdir = std::string(cdir) + "/branches";
    /* A link in place of branches/ is not followed (the slot cleanup refuses
     * it too): what it points at is outside the cache. */
    cbm_dir_t *bd = NULL;
    if (cbm_is_link(bdir.c_str())) {
        cbm_log_warn("cache.expire.refused", "reason", "branches directory is a link", "path",
                     bdir.c_str());
    } else {
        bd = cbm_opendir(bdir.c_str());
    }
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
        if (kept(opts, p.c_str()) || c.previewed.count(p)) {
            continue;
        }
        cbm_db_lease_t *lease = NULL;
        if ((opts->dry_run ? cbm_db_lease_probe(dbp.c_str())
                           : cbm_db_lease_try_acquire(dbp.c_str(), &lease)) != 0) {
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
        if (opts->dry_run && cbm_db_lease_probe(dbp.c_str()) != 0) {
            continue; /* an index run creating the database holds its lease */
        }
        if (!opts->dry_run) {
            cbm_db_lease_t *lease = NULL;
            if (cbm_db_lease_try_acquire(dbp.c_str(), &lease) != 0) {
                continue;
            }
            /* Reported only once the sidecar is gone: its database must
             * still be absent under the lease, and the unlink must have
             * taken (a read-only cache refuses it). */
            bool gone = !cbm_file_exists(dbp.c_str()) &&
                        (cbm_unlink(sp.c_str()) == 0 || !cbm_file_exists(sp.c_str()));
            cbm_db_lease_release(lease);
            if (!gone) {
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
    if (!trash_dir_usable(tdir)) {
        return out;
    }
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

int cbm_cache_trash_remove_all(void) {
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir || !cdir[0]) {
        return 0;
    }
    std::string tdir = std::string(cdir) + "/trash";
    walk_kind_t self = walk_kind(tdir.c_str());
    if (self != WALK_DIR) {
        if (self == WALK_LINK) {
            remove_link(tdir.c_str()); /* never what it points at */
        }
        return 0;
    }
    std::vector<std::string> names;
    cbm_dir_t *d = cbm_opendir(tdir.c_str());
    if (d) {
        cbm_dirent_t *ent;
        while ((ent = cbm_readdir(d)) != NULL) {
            if (strcmp(ent->name, ".") != 0 && strcmp(ent->name, "..") != 0) {
                names.emplace_back(ent->name);
            }
        }
        cbm_closedir(d);
    }
    int removed = 0;
    for (const auto &n : names) {
        std::string p = tdir + "/" + n;
        /* An entry is restored under its project's lease, as the sweep
         * purges it: one being restored right now stays. */
        cbm_db_lease_t *lease = NULL;
        size_t dash = n.find('-');
        bool entry = dash != std::string::npos && walk_kind(p.c_str()) == WALK_DIR;
        if (entry) {
            std::string dbp = std::string(cdir) + "/" + n.substr(dash + 1) + ".db";
            if (cbm_db_lease_try_acquire(dbp.c_str(), &lease) != 0) {
                continue;
            }
        }
        bool gone = remove_tree(p.c_str());
        cbm_db_lease_release(lease);
        removed += entry && gone;
    }
    (void)cbm_rmdir(tdir.c_str()); /* only when empty */
    return removed;
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
        /* The saved WAL goes back first, the main file last: the database
         * is published by the main file's rename, and a writer opening it
         * in between (an ADR write from another server) would have its own
         * fresh WAL replaced by the saved one. With the WAL in place before
         * the main file, an opener finds both. */
        std::string wal = src + "-wal";
        std::string live_wal = dbp + "-wal";
        bool wal_back = !cbm_file_exists(wal.c_str()) ||
                        cbm_rename_replace(wal.c_str(), live_wal.c_str()) == 0;
        if (!wal_back || cbm_rename_replace(src.c_str(), dbp.c_str()) != 0) {
            snprintf(msg, msg_sz, "could not move %s back", wal_back ? src.c_str() : wal.c_str());
            if (wal_back && cbm_file_exists(live_wal.c_str())) {
                (void)cbm_rename_replace(live_wal.c_str(), wal.c_str()); /* undo */
            }
        } else {
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
                (void)remove_tree(edir.c_str()); /* a leftover entry is purged later */
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
