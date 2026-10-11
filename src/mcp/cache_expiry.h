/*
 * cache_expiry.h — Remove index databases nobody uses any more.
 *
 * The cache otherwise only grows: <cache>/<project>.db stays after its
 * checkout is deleted (a `git worktree remove`, a scratch clone), parked
 * branch slots stay until their project's count or size cap pushes them out,
 * and an interrupted run can leave SQLite sidecars and temp files behind.
 * A sweep finds four kinds of stale entries:
 *
 *   root_gone  the recorded checkout root is missing on N sweeps spread over
 *              a grace period, and nothing used the database in that period;
 *   unused     no tool call, hook or index run touched the database for a
 *              long time (cache_usage.last_used);
 *   slot       a parked branch slot parked long ago and never restored;
 *   sidecar    a -wal/-shm/.tmp file whose database is gone, or a scratch
 *              file of an interrupted park or restore.
 *
 * A database that goes is MOVED into <cache>/trash/<epoch>-<project>/ with its
 * parked slots, and deleted by a sweep at least a day later; until then
 * `code-cortex-mcp cache restore` puts it back. Slots and sidecars are caches
 * and are deleted directly. Every removal holds the project's index lease.
 *
 * Usage is recorded in table cache_usage of <cache>/_config.db: resolve_store
 * (every tool call and hook) and every index run touch the row, at most once
 * an hour per database (an in-process throttle, then one indexed read).
 *
 * Design and trade-offs: docs/CACHE_EXPIRY.md.
 *
 * Depends on: store_meta, pipeline (branch_slots), cli (config), foundation
 */
#ifndef CBM_CACHE_EXPIRY_H
#define CBM_CACHE_EXPIRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cbm_config;

enum {
    CBM_CACHE_DAY_S = 86400,
    /* Defaults; config keys in cli.h (CBM_CONFIG_CACHE_EXPIRY_*). */
    CBM_CACHE_EXPIRY_UNUSED_DAYS = 90,
    CBM_CACHE_EXPIRY_ROOT_GRACE_DAYS = 7,
    CBM_CACHE_EXPIRY_SLOT_DAYS = 30,
    CBM_CACHE_EXPIRY_ROOT_CHECKS = 3,   /* missing on this many sweeps ... */
    CBM_CACHE_EXPIRY_INTERVAL_S = 86400, /* automatic sweeps at most daily */
    CBM_CACHE_EXPIRY_TRASH_S = 86400,    /* trash entries kept at least this long */
    CBM_CACHE_EXPIRY_SCRATCH_S = 3600,   /* orphan temp files older than this */
    CBM_CACHE_USAGE_RESOLUTION_S = 3600, /* last_used written at most hourly */
};

typedef struct {
    bool enabled;         /* automatic sweeps (config cache_expiry, CBM_CACHE_EXPIRY) */
    int64_t unused_s;     /* <= 0: rule off */
    int64_t root_grace_s; /* <= 0: rule off */
    int root_checks;
    int64_t slot_s; /* <= 0: rule off */
    int64_t trash_s;
    int64_t scratch_s;
    int64_t interval_s;
} cbm_cache_expiry_cfg_t;

/* Defaults, then the config keys (cfg NULL: <cache>/_config.db is opened for
 * the read), then CBM_CACHE_EXPIRY=0 (automatic sweeps off). */
void cbm_cache_expiry_cfg_load(cbm_cache_expiry_cfg_t *out, struct cbm_config *cfg);

/* Record that the database at db_path was used now (root_path, when known,
 * is recorded as its checkout root and clears a missing-root streak). Cheap:
 * an in-process throttle, then one read of _config.db; it writes at most once
 * per CBM_CACHE_USAGE_RESOLUTION_S per database. Best-effort, never fails. */
void cbm_cache_usage_touch(const char *db_path, const char *root_path);

/* Forget the database at db_path: its cache_usage and store_meta rows, and
 * this process's throttle entry. Called wherever a database is removed, so
 * a database later built at that path starts clean instead of inheriting a
 * stale root or missing-root streak. */
void cbm_cache_usage_forget(const char *db_path);

/* Set a database's usage row outright (tests, `cache restore`). last_used 0
 * leaves it; root_path NULL leaves it. Resets the missing-root streak. */
bool cbm_cache_usage_set(const char *db_path, int64_t last_used, const char *root_path);

typedef enum {
    CBM_EXPIRE_ROOT_GONE = 0,
    CBM_EXPIRE_UNUSED,
    CBM_EXPIRE_SLOT,
    CBM_EXPIRE_SIDECAR,
    CBM_EXPIRE_TRASH_PURGE,
    CBM_EXPIRE_PENDING, /* root missing, still in its grace period: reported only */
} cbm_cache_expiry_kind_t;

const char *cbm_cache_expiry_kind_str(cbm_cache_expiry_kind_t kind);

typedef struct {
    cbm_cache_expiry_kind_t kind;
    const char *name;   /* project, or file name for a sidecar/slot */
    const char *path;   /* the file or directory acted on */
    int64_t bytes;      /* what it frees (database + WAL + slots for a project) */
    const char *reason; /* one line for humans */
    bool removed;       /* false in a dry run, for PENDING, or when skipped */
    const char *skipped; /* why it was not removed (busy, in use); NULL otherwise */
} cbm_cache_expiry_item_t;

typedef struct {
    int64_t now;  /* epoch seconds; 0 = time(NULL) */
    bool dry_run; /* decide and report only: no removal, no state written */
    /* Never expire these projects: this session's, and (keep) the ones a
     * watcher in this process watches. */
    const char *keep_project;
    bool (*keep)(const char *project, void *ud);
    void *keep_ud;
    void (*report)(const cbm_cache_expiry_item_t *item, void *ud);
    void *report_ud;
} cbm_cache_sweep_opts_t;

typedef struct {
    int projects;      /* databases removed (or due, in a dry run) */
    int slots;         /* parked slots removed (or due) */
    int sidecars;      /* orphan files removed (or due) */
    int purged;        /* trash entries deleted (or due) */
    int skipped;       /* due but kept: busy, in use */
    int64_t bytes;     /* freed (or to be freed) */
    bool refused;      /* the sweep did not run */
    char refused_reason[128];
} cbm_cache_sweep_result_t;

/* One sweep of the cache directory. Returns 0, or -1 when it refused to run
 * (result->refused_reason says why: another sweep holds the lock, the cache
 * directory or HOME looks wrong). */
int cbm_cache_expiry_sweep(const cbm_cache_expiry_cfg_t *cfg, const cbm_cache_sweep_opts_t *opts,
                           cbm_cache_sweep_result_t *result);

/* Whether an automatic sweep is due at `now` (enabled, and the last sweep
 * recorded in _config.db is at least cfg->interval_s old). */
bool cbm_cache_expiry_due(const cbm_cache_expiry_cfg_t *cfg, int64_t now);

/* Remove project's database at db_path, its WAL and SHM, its parked slots
 * (every root's), its store_meta and cache_usage rows. trash_entry NULL
 * deletes; otherwise the database (and WAL) and the slot directory are moved
 * into that directory. The caller holds the index lease. Returns 0 when the
 * database was removed, 1 when it did not exist, -1 when it could not be
 * removed (its slots are then kept), 2 when it was removed but some parked
 * slot could not be (cache.remove.slots_left; the slot expiry retries).
 * *parked receives the slot count removed or moved. */
int cbm_cache_remove_project(const char *db_path, const char *project, const char *trash_entry,
                             int *parked);

/* Put a trashed database back: `which` is a trash entry name
 * ("<epoch>-<project>") or a project name (its newest entry). Fails when the
 * project has a database again. msg receives a one-line outcome. */
int cbm_cache_trash_restore(const char *which, char *msg, size_t msg_sz);

/* Delete the whole trash directory, never following a link (uninstall).
 * An entry whose project is being restored or indexed (its lease busy) is
 * kept. Returns the trash entries removed. */
int cbm_cache_trash_remove_all(void);

/* Trash entries, newest first, printed one per line to `out` (a FILE *). */
int cbm_cache_trash_list(void *out);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Callback run once by a sweep after it has read and judged every usage row
 * and before it writes any. NULL clears. */
void cbm_cache_expiry_test_scan_hook(void (*callback)(void *data), void *data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CBM_CACHE_EXPIRY_H */
