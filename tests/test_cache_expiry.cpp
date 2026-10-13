/*
 * test_cache_expiry.cpp — Removal of stale index databases (mcp/cache_expiry.h).
 *
 * Each test runs the sweep against its own cache directory (CBM_CACHE_DIR)
 * with an explicit clock, on hand-made project databases that hold only the
 * projects table the sweep reads.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <foundation/compat_fs.h>
#include <foundation/constants.h>
#include <foundation/platform.h>
#include <mcp/cache_expiry.h>
#include <mcp/mcp.h>
#include <mcp/store_meta.h>
#include <pipeline/branch_slots.h>
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <watcher/watcher.h>

#include "sqlite3.h"
#include <atomic>
#include <chrono>
#include <ctype.h>
#include <errno.h>
#include <new>
#include <thread>
#include <vector>
#include <string>
#include <thread>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#endif

enum { CE_DAY = 86400 };

typedef struct {
    char cache[512];
    char base[512];
    char old_cache[1024];
    bool had_cache;
} ce_env_t;

/* A fresh cache dir (with _config.db) and a scratch base for roots. */
static bool ce_setup(ce_env_t *e, const char *prefix) {
    memset(e, 0, sizeof(*e));
    char *b = th_mktempdir(prefix);
    if (!b) {
        return false;
    }
    snprintf(e->base, sizeof(e->base), "%s", b);
    snprintf(e->cache, sizeof(e->cache), "%s/cache", e->base);
    cbm_mkdir_p(e->cache, 0755);
    e->had_cache = cbm_safe_getenv("CBM_CACHE_DIR", e->old_cache, sizeof(e->old_cache), NULL);
    cbm_setenv("CBM_CACHE_DIR", e->cache, 1);
    cbm_config_t *cfg = cbm_config_open(e->cache);
    cbm_config_close(cfg);
    return true;
}

static void ce_teardown(ce_env_t *e) {
    if (e->had_cache) {
        cbm_setenv("CBM_CACHE_DIR", e->old_cache, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_rmtree(e->base);
}

/* <cache>/<file>.db holding project `name` (a renamed or legacy file when
 * they differ) recording root, in WAL mode. */
static void ce_make_db_as(const ce_env_t *e, const char *file, const char *name, const char *root,
                          char *path, size_t n) {
    snprintf(path, n, "%s/%s.db", e->cache, file);
    sqlite3 *db = NULL;
    sqlite3_open(path, &db);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    char sql[2048];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE projects(name TEXT PRIMARY KEY, indexed_at TEXT, root_path TEXT);"
             "INSERT INTO projects VALUES('%s','2026-01-01','%s');",
             name, root);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
}

/* <cache>/<name>.db recording root, in WAL mode. */
static void ce_make_db(const ce_env_t *e, const char *name, const char *root, char *path,
                       size_t n) {
    ce_make_db_as(e, name, name, root, path, n);
}

static void ce_cfg(cbm_cache_expiry_cfg_t *cfg) {
    cbm_cache_expiry_cfg_load(cfg, NULL);
    cfg->enabled = true;
}

static int ce_sweep(const cbm_cache_expiry_cfg_t *cfg, int64_t now, bool dry,
                    cbm_cache_sweep_result_t *res, const char *keep = NULL) {
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.dry_run = dry;
    o.keep_project = keep;
    return cbm_cache_expiry_sweep(cfg, &o, res);
}

static int ce_trash_count(const ce_env_t *e) {
    char t[700];
    snprintf(t, sizeof(t), "%s/trash", e->cache);
    cbm_dir_t *d = cbm_opendir(t);
    if (!d) {
        return 0;
    }
    int n = 0;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        n += ent->is_dir && ent->name[0] != '.';
    }
    cbm_closedir(d);
    return n;
}

/* The usage row's last_used and missing_checks (-1 when absent). */
static void ce_usage(const ce_env_t *e, const char *name, int64_t *last_used, int *checks) {
    *last_used = -1;
    *checks = -1;
    char p[700];
    snprintf(p, sizeof(p), "%s/_config.db", e->cache);
    sqlite3 *db = NULL;
    sqlite3_open(p, &db);
    sqlite3_stmt *st = NULL;
    char key[300];
    snprintf(key, sizeof(key), "%s.db", name);
    if (sqlite3_prepare_v2(db, "SELECT last_used, missing_checks FROM cache_usage WHERE db_name=?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *last_used = sqlite3_column_int64(st, 0);
            *checks = sqlite3_column_int(st, 1);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

#ifndef _WIN32
static void ce_age(const char *path, int64_t when) {
    struct utimbuf ut = {(time_t)when, (time_t)when};
    utime(path, &ut);
}
#endif

/* A removed checkout expires only after N sweeps spread over the grace
 * period, and only once nothing used its database for that long. */
TEST(cache_root_gone_needs_streak_and_grace) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_gone"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "gone", root, db, sizeof(db));
    th_rmtree(root); /* git worktree remove */
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    int64_t lu;
    int checks;

    for (int day = 0; day < 3; day++) { /* three sweeps, inside the grace */
        ASSERT_EQ(ce_sweep(&cfg, t0 + day * CE_DAY, false, &r), 0);
        ASSERT_EQ(r.projects, 0);
        ASSERT_TRUE(cbm_file_exists(db));
    }
    ce_usage(&e, "gone", &lu, &checks);
    ASSERT_EQ(checks, 3);
    ASSERT_EQ(lu, t0); /* first sight starts the clock */
    ASSERT_EQ(ce_sweep(&cfg, t0 + 8 * CE_DAY, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 1);
    ce_usage(&e, "gone", &lu, &checks);
    ASSERT_EQ(lu, -1); /* the row went with the database */
    ce_teardown(&e);
    PASS();
}

/* A root that comes back between the scan and the move (a remount; the
 * keep callback runs in that window) keeps the database: the verdict is
 * checked again under the reader gate. */
static bool ce_restore_root_between(const char *project, void *ud) {
    (void)project;
    cbm_mkdir_p((const char *)ud, 0755);
    return false;
}

TEST(cache_root_back_before_move_is_kept) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_remount"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    ce_make_db(&e, "remounted", root, db, sizeof(db)); /* root absent */
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    for (int day = 0; day < 3; day++) {
        ASSERT_EQ(ce_sweep(&cfg, t0 + day * CE_DAY, false, &r), 0);
    }
    cbm_cache_sweep_opts_t o = {};
    o.now = t0 + 8 * CE_DAY;
    o.keep = ce_restore_root_between;
    o.keep_ud = root;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    ce_teardown(&e);
    PASS();
}

/* `cache expire --now` is a preview only: without --dry-run it is refused
 * and nothing is swept (a sweep would date its trash entries and rows with
 * that clock). */
TEST(cache_cli_now_needs_dry_run) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_clinow"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "clinow", root, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    int64_t future = (int64_t)time(NULL) + 400 * CE_DAY;
    char nowbuf[32];
    snprintf(nowbuf, sizeof(nowbuf), "%lld", (long long)future);
    char a0[] = "expire";
    char a1[] = "--now";
    char a2[] = "--dry-run";
    char *real[] = {a0, a1, nowbuf};
    ASSERT_NEQ(cbm_cmd_cache(3, real), 0);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    ASSERT_TRUE(cbm_cache_expiry_due(&cfg, (int64_t)time(NULL))); /* nothing swept */
    char *preview[] = {a0, a2, a1, nowbuf};
    ASSERT_EQ(cbm_cmd_cache(4, preview), 0);
    ASSERT_TRUE(cbm_file_exists(db));
    ce_teardown(&e);
    PASS();
}

/* Uninstall's index removal takes the trash with it, never following a
 * link out of it. No usage or memo row survives either: they live in
 * _config.db, which goes with the other *.db files. */
TEST(cache_uninstall_removes_trash_and_rows) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_uninst"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "uninst", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, (int64_t)time(NULL), root));
    cbm_file_gen_t gen;
    ASSERT_TRUE(cbm_file_generation(db, &gen));
    cbm_store_meta_row_t row;
    cbm_store_meta_row_init(&row, db, &gen);
    snprintf(row.project, sizeof(row.project), "uninst");
    snprintf(row.root_path, sizeof(row.root_path), "%s", root);
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    ASSERT_TRUE(cbm_store_meta_put(meta, &row));
    cbm_store_meta_close(meta);
    /* A trash entry holding a database, a link to a directory outside the
     * cache, and a link to a file outside it. */
    char outside[800];
    snprintf(outside, sizeof(outside), "%s/outside", e.base);
    cbm_mkdir_p(outside, 0755);
    char keep[900];
    snprintf(keep, sizeof(keep), "%s/precious.txt", outside);
    ASSERT_EQ(th_write_file(keep, "keep"), 0);
    char entry[900];
    snprintf(entry, sizeof(entry), "%s/trash/1700000000-old", e.cache);
    ASSERT_TRUE(cbm_mkdir_p(entry, 0755));
    char p[1000];
    snprintf(p, sizeof(p), "%s/old.db", entry);
    ASSERT_EQ(th_write_file(p, "old graph"), 0);
    snprintf(p, sizeof(p), "%s/dirlink", entry);
    ASSERT_EQ(symlink(outside, p), 0);
    snprintf(p, sizeof(p), "%s/filelink", entry);
    ASSERT_EQ(symlink(keep, p), 0);

    ASSERT_GTE(cbm_remove_indexes(e.base), 1);
    ASSERT_FALSE(cbm_file_exists(db));
    int64_t lu;
    int checks;
    ce_usage(&e, "uninst", &lu, &checks);
    ASSERT_EQ(lu, -1);
    meta = cbm_store_meta_open();
    cbm_store_meta_row_t found;
    bool memo = cbm_store_meta_find_project(meta, "uninst", &found);
    cbm_store_meta_close(meta);
    ASSERT_FALSE(memo);
    snprintf(p, sizeof(p), "%s/trash", e.cache);
    ASSERT_FALSE(cbm_is_dir(p));
    ASSERT_TRUE(cbm_file_exists(keep));
    ce_teardown(&e);
#endif
    PASS();
}

/* A use inside the grace period keeps a database whose root is gone (a
 * project queried by name after its checkout moved). */
TEST(cache_root_gone_but_used_is_kept) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_used"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    ce_make_db(&e, "used", root, db, sizeof(db)); /* root never existed */
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    for (int day = 0; day < 3; day++) {
        ASSERT_EQ(ce_sweep(&cfg, t0 + day * CE_DAY, false, &r), 0);
    }
    ASSERT_TRUE(cbm_cache_usage_set(db, t0 + 6 * CE_DAY, NULL)); /* also resets the streak */
    ASSERT_EQ(ce_sweep(&cfg, t0 + 8 * CE_DAY, false, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_TRUE(cbm_file_exists(db));
    ce_teardown(&e);
    PASS();
}

/* A root that comes back restarts the streak. */
TEST(cache_root_back_resets_streak) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_back"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    ce_make_db(&e, "back", root, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    int64_t lu;
    int checks;
    ASSERT_EQ(ce_sweep(&cfg, t0, false, &r), 0);
    ASSERT_EQ(ce_sweep(&cfg, t0 + CE_DAY, false, &r), 0);
    ce_usage(&e, "back", &lu, &checks);
    ASSERT_EQ(checks, 2);
    cbm_mkdir_p(root, 0755); /* the volume came back */
    ASSERT_EQ(ce_sweep(&cfg, t0 + 2 * CE_DAY, false, &r), 0);
    ce_usage(&e, "back", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_teardown(&e);
    PASS();
}

/* A root under a volume mount point or "/" itself (an unmounted disk), and
 * a relative root, never count as missing. */
TEST(cache_root_uncertain_never_counts) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_vol"));
    char db[800];
    ce_make_db(&e, "vol", "/Volumes/cbm-no-such-volume/repo", db, sizeof(db));
    ce_make_db(&e, "mnt", "/mnt/cbm-no-such-mount/work/repo", db, sizeof(db));
    ce_make_db(&e, "top", "/cbm-no-such-top-dir/repo", db, sizeof(db));
    ce_make_db(&e, "rel", "relative/repo", db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    for (int day = 0; day <= 30; day += 3) {
        ASSERT_EQ(ce_sweep(&cfg, t0 + day * CE_DAY, false, &r), 0);
        ASSERT_EQ(r.projects, 0);
    }
    int64_t lu;
    int checks;
    const char *names[] = {"vol", "mnt", "top", "rel"};
    for (const char *n : names) {
        ce_usage(&e, n, &lu, &checks);
        ASSERT_EQ(checks, 0);
    }
    ce_teardown(&e);
    PASS();
}

/* A memo root that fills its column may be truncated: the sweep reads the
 * database's own projects table instead of starting a missing streak
 * against a live checkout. */
TEST(cache_truncated_memo_root_is_not_trusted) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_memolong"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "ml", root, db, sizeof(db));
    cbm_file_gen_t gen;
    ASSERT_TRUE(cbm_file_generation(db, &gen));
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    ASSERT_NOT_NULL(meta);
    cbm_store_meta_row_t row;
    cbm_store_meta_row_init(&row, db, &gen);
    snprintf(row.project, sizeof(row.project), "ml");
    memset(row.root_path, 'x', sizeof(row.root_path) - 1);
    row.root_path[0] = '/';
    row.root_path[sizeof(row.root_path) - 1] = '\0'; /* a root cut at the column */
    ASSERT_TRUE(cbm_store_meta_put(meta, &row));
    cbm_store_meta_close(meta);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    ASSERT_EQ(r.projects, 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "ml", &lu, &checks);
    ASSERT_EQ(checks, 0); /* the real root exists: no streak */
    ASSERT_TRUE(cbm_file_exists(db));
    ce_teardown(&e);
    PASS();
}

/* A root whose nearest existing ancestor is an empty directory directly in
 * a mount parent (a mount point with nothing mounted, /mnt/data) is
 * unavailable, not deleted: no streak. One whose ancestor has other entries
 * (a removed checkout beside others) is missing. The test makes its base a
 * mount parent (like /mnt), which it could not create. */
TEST(cache_root_under_empty_ancestor_is_uncertain) {
#ifdef CBM_ENABLE_TEST_SEAMS
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_mnt"));
    cbm_cache_expiry_test_mount_parent(e.base);
    char mnt[700];
    char busy[700];
    snprintf(mnt, sizeof(mnt), "%s/mnt-data", e.base);
    snprintf(busy, sizeof(busy), "%s/work", e.base);
    ASSERT_TRUE(cbm_mkdir_p(mnt, 0755));
    ASSERT_TRUE(cbm_mkdir_p(busy, 0755));
    char sibling[800];
    snprintf(sibling, sizeof(sibling), "%s/other-repo", busy);
    ASSERT_TRUE(cbm_mkdir_p(sibling, 0755));
    char r1[800];
    char r2[800];
    snprintf(r1, sizeof(r1), "%s/repo", mnt);
    snprintf(r2, sizeof(r2), "%s/repo", busy);
    char db[800];
    ce_make_db(&e, "unmounted", r1, db, sizeof(db));
    ce_make_db(&e, "deleted", r2, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    int64_t lu;
    int checks;
    cbm_cache_expiry_test_mount_parent(NULL);
    ce_usage(&e, "unmounted", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_usage(&e, "deleted", &lu, &checks);
    ASSERT_EQ(checks, 1);
    ce_teardown(&e);
#endif
    PASS();
}

/* `git worktree remove` of repo/.worktrees/x leaves repo/.worktrees empty.
 * An empty directory that is not directly in a mount parent is no unmounted
 * mount point: the worktree is missing, and expires by root_gone. */
TEST(cache_removed_worktree_under_empty_dir_is_missing) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_wtgone"));
    char wts[700];
    snprintf(wts, sizeof(wts), "%s/repo/.worktrees", e.base);
    ASSERT_TRUE(cbm_mkdir_p(wts, 0755)); /* left empty */
    char wt[800];
    snprintf(wt, sizeof(wt), "%s/x", wts);
    char db[800];
    ce_make_db(&e, "wtx", wt, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "wtx", &lu, &checks);
    ASSERT_EQ(checks, 1);
    ce_teardown(&e);
    PASS();
}

/* A directory link at link pointing to target: a symlink, or on Windows a
 * junction (no privilege needed). False when the system refused one. */
static bool ce_dir_link(const char *link, const char *target) {
#ifdef _WIN32
    char wl[800];
    char wt[800];
    snprintf(wl, sizeof(wl), "%s", link);
    snprintf(wt, sizeof(wt), "%s", target);
    for (char *c = wl; *c; c++) {
        *c = *c == '/' ? '\\' : *c;
    }
    for (char *c = wt; *c; c++) {
        *c = *c == '/' ? '\\' : *c;
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1", wl, wt);
    return system(cmd) == 0 && cbm_is_link(link);
#else
    return symlink(target, link) == 0;
#endif
}

static void ce_dir_unlink(const char *link) {
#ifdef _WIN32
    (void)cbm_rmdir(link);
#else
    (void)cbm_unlink(link);
#endif
}

/* A root reached through a link to a volume that is not mounted (the link
 * dangles) is unavailable, not deleted: no streak, whether the link is an
 * ancestor of the root or the root itself. */
TEST(cache_root_behind_dangling_link_is_uncertain) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_dangle"));
    char work[700];
    char sibling[800];
    char vol[700];
    char volrepo[800];
    snprintf(work, sizeof(work), "%s/work", e.base);
    snprintf(sibling, sizeof(sibling), "%s/other-repo", work);
    snprintf(vol, sizeof(vol), "%s/volume", e.base);
    snprintf(volrepo, sizeof(volrepo), "%s/repo", vol);
    ASSERT_TRUE(cbm_mkdir_p(sibling, 0755));
    ASSERT_TRUE(cbm_mkdir_p(volrepo, 0755));
    char via[800];
    char direct[800];
    snprintf(via, sizeof(via), "%s/ext", work);
    snprintf(direct, sizeof(direct), "%s/linked-repo", work);
    if (!ce_dir_link(via, vol) || !ce_dir_link(direct, volrepo)) {
        ce_dir_unlink(via);
        ce_teardown(&e);
        PASS(); /* no links on this filesystem */
    }
    th_rmtree(vol); /* the volume goes away; both links dangle */
    char r1[900];
    char r2[900];
    snprintf(r1, sizeof(r1), "%s/repo", via);
    snprintf(r2, sizeof(r2), "%s/gone-repo", work);
    char db[800];
    ce_make_db(&e, "via", r1, db, sizeof(db));
    ce_make_db(&e, "direct", direct, db, sizeof(db));
    ce_make_db(&e, "deleted", r2, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "via", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_usage(&e, "direct", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_usage(&e, "deleted", &lu, &checks);
    ASSERT_EQ(checks, 1); /* a plain missing root still counts */
    ce_dir_unlink(via);
    ce_dir_unlink(direct);
    ce_teardown(&e);
    PASS();
}

/* A root named with non-ASCII characters, and one past the Windows MAX_PATH,
 * is present: the probe must not read either as "no such file" (the narrow
 * Windows stat does) and start a missing streak on a live checkout. */
TEST(cache_root_unicode_and_long_is_present) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_uni"));
    char uni[700];
    snprintf(uni, sizeof(uni), "%s/r\xC3\xA9po-\xE6\x97\xA5\xE6\x9C\xAC", e.base); /* répo-日本 */
    ASSERT_TRUE(cbm_mkdir_p(uni, 0755));
    std::string longroot = e.base;
    while (longroot.size() < 300) {
        longroot += "/segment-of-a-long-checkout-path";
    }
    ASSERT_TRUE(cbm_mkdir_p(longroot.c_str(), 0755));
    char db[800];
    ce_make_db(&e, "uni", uni, db, sizeof(db));
    ce_make_db(&e, "long", longroot.c_str(), db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    for (int day = 0; day < 3; day++) {
        ASSERT_EQ(ce_sweep(&cfg, t0 + day * CE_DAY, false, &r), 0);
    }
    int64_t lu;
    int checks;
    ce_usage(&e, "uni", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_usage(&e, "long", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_teardown(&e);
    PASS();
}

/* Every recorded root missing at once (a moved HOME, a copied cache) counts
 * nothing; so does a sweep from another HOME. */
TEST(cache_mass_missing_and_env_change_count_nothing) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_mass"));
    char db[800];
    char root[700];
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "m%d", i);
        snprintf(root, sizeof(root), "%s/gone%d", e.base, i);
        ce_make_db(&e, name, root, db, sizeof(db));
    }
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    int64_t lu;
    int checks;
    ASSERT_EQ(ce_sweep(&cfg, t0, false, &r), 0);
    ce_usage(&e, "m0", &lu, &checks);
    ASSERT_EQ(checks, 0);

    /* One root present: the others count. */
    snprintf(root, sizeof(root), "%s/gone4", e.base);
    cbm_mkdir_p(root, 0755);
    ASSERT_EQ(ce_sweep(&cfg, t0 + CE_DAY, false, &r), 0);
    ce_usage(&e, "m0", &lu, &checks);
    ASSERT_EQ(checks, 1);

    /* Another HOME: streaks restart, nothing counted. */
    char old_home[1024] = "";
    cbm_safe_getenv("HOME", old_home, sizeof(old_home), NULL);
    char home2[700];
    snprintf(home2, sizeof(home2), "%s/home2", e.base);
    cbm_mkdir_p(home2, 0755);
    cbm_setenv("HOME", home2, 1);
    ASSERT_EQ(ce_sweep(&cfg, t0 + 2 * CE_DAY, false, &r), 0);
    cbm_setenv("HOME", old_home, 1);
    ce_usage(&e, "m0", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_teardown(&e);
    PASS();
}

/* No _config.db: not this user's cache, or a fresh one. */
TEST(cache_refuses_unknown_cache_dir) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_refuse"));
    char cfgdb[800];
    snprintf(cfgdb, sizeof(cfgdb), "%s/_config.db", e.cache);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg); /* before: loading the config creates _config.db */
    ASSERT_EQ(cbm_unlink(cfgdb), 0);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, 0, false, &r), -1);
    ASSERT_TRUE(r.refused);
    ce_teardown(&e);
    PASS();
}

/* Unused for the threshold: trashed. Used recently, or first seen now:
 * kept. Disabled rule (0 days): kept. */
TEST(cache_unused_expires) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_unused"));
    char root[700];
    char stale[800];
    char fresh[800];
    char unseen[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "stale", root, stale, sizeof(stale));
    ce_make_db(&e, "fresh", root, fresh, sizeof(fresh));
    ce_make_db(&e, "unseen", root, unseen, sizeof(unseen));
#ifndef _WIN32
    ce_age(unseen, now - 400 * CE_DAY); /* an old file: its age is not use */
#endif
    ASSERT_TRUE(cbm_cache_usage_set(stale, now - 91 * CE_DAY, NULL));
    ASSERT_TRUE(cbm_cache_usage_set(fresh, now - 10 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    cfg.unused_s = 0;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ce_cfg(&cfg);
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(stale));
    ASSERT_TRUE(cbm_file_exists(fresh));
    ASSERT_TRUE(cbm_file_exists(unseen));
    ce_teardown(&e);
    PASS();
}

/* A *.db in the cache directory that is no index of ours -- not SQLite at
 * all, or an SQLite file without a projects table -- is never judged by its
 * filename, however long unused: files this module did not write stay. */
TEST(cache_foreign_db_files_are_not_judged) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_foreign"));
    char root[700];
    char stale[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "stale", root, stale, sizeof(stale));
    char text[800];
    char other[800];
    snprintf(text, sizeof(text), "%s/notes.db", e.cache);
    snprintf(other, sizeof(other), "%s/tool-state.db", e.cache);
    ASSERT_EQ(th_write_file(text, "not an sqlite database"), 0);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open(other, &db), SQLITE_OK);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_close(db);
    const char *paths[] = {stale, text, other};
    for (const char *p : paths) {
        ASSERT_TRUE(cbm_cache_usage_set(p, now - 200 * CE_DAY, NULL));
    }
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, true, &r), 0);
    ASSERT_EQ(r.projects, 1); /* only the index, in the preview too */
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(stale));
    ASSERT_TRUE(cbm_file_exists(text));
    ASSERT_TRUE(cbm_file_exists(other));
    ASSERT_EQ(ce_trash_count(&e), 1);
    ce_teardown(&e);
    PASS();
}

/* The session's project and the watched ones are never expired. */
static bool ce_keep_w(const char *project, void *ud) {
    return strcmp(project, (const char *)ud) == 0;
}

TEST(cache_keeps_session_and_watched) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_keep"));
    char root[700];
    char a[800];
    char b[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "sess", root, a, sizeof(a));
    ce_make_db(&e, "watched", root, b, sizeof(b));
    ASSERT_TRUE(cbm_cache_usage_set(a, now - 200 * CE_DAY, NULL));
    ASSERT_TRUE(cbm_cache_usage_set(b, now - 200 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.keep_project = "sess";
    o.keep = ce_keep_w;
    o.keep_ud = (void *)"watched";
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 2);
    ASSERT_TRUE(cbm_file_exists(a));
    ASSERT_TRUE(cbm_file_exists(b));
    ce_teardown(&e);
    PASS();
}

/* A Windows root is stored with backslashes: once the checkout is gone, the
 * walk up to its nearest existing ancestor must split on them, or the root is
 * classed uncertain and its missing streak never starts. */
TEST(cache_root_gone_windows_separators) {
#ifdef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_bs"));
    char root[700];
    snprintf(root, sizeof(root), "%s\\gone\\repo", e.base);
    for (char *c = root; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    char db[800];
    ce_make_db(&e, "bs", root, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    ASSERT_EQ(ce_sweep(&cfg, t0, false, &r), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "bs", &lu, &checks);
    ASSERT_EQ(checks, 1); /* missing, not uncertain */
    ce_teardown(&e);
#endif
    PASS();
}

static void ce_count_slot(const char *path, int64_t bytes, bool scratch, void *ud) {
    (void)path;
    (void)bytes;
    (void)scratch;
    (*(int *)ud)++;
}

/* A slot whose unlink is refused (a read-only slot directory) is neither
 * reported nor counted as removed. */
TEST(cache_slot_unlink_refused_not_counted) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root ignores directory permissions */
    }
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_ro"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "ro", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "ro", root, sdir, sizeof(sdir)));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/old.db.zst", sdir);
    th_write_file(slot, "a");
    ce_age(slot, now - 31 * CE_DAY);
    ASSERT_EQ(chmod(sdir, 0555), 0);
    int reported = 0;
    int removed = cbm_branch_slots_expire(db, "ro", now - 30 * CE_DAY, now - 3600, false,
                                          ce_count_slot, &reported);
    chmod(sdir, 0755);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(reported, 0);
    ASSERT_TRUE(cbm_file_exists(slot));
    /* Writable again: reported and counted once it is gone. */
    removed = cbm_branch_slots_expire(db, "ro", now - 30 * CE_DAY, now - 3600, false,
                                      ce_count_slot, &reported);
    ASSERT_EQ(removed, 1);
    ASSERT_EQ(reported, 1);
    ASSERT_FALSE(cbm_file_exists(slot));
    ce_teardown(&e);
#endif
    PASS();
}

/* A preview decides with the sweep's own checks: a project being indexed
 * (its lease held) is reported kept, not "would be removed". And loading
 * the expiry settings for a cache that does not exist creates nothing. */
TEST(cache_dry_run_applies_safety_checks_and_creates_nothing) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_dryck"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "busy", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(db, &lease), 0);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, true, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1); /* "being indexed", as the real sweep would say */
    /* An aged slot of the leased project is not previewed as removable. */
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "busy", root, sdir, sizeof(sdir)));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/old.db.zst", sdir);
    th_write_file(slot, "s");
#ifndef _WIN32
    ce_age(slot, now - 40 * CE_DAY);
    ASSERT_EQ(ce_sweep(&cfg, now, true, &r), 0);
    ASSERT_EQ(r.slots, 0);
#endif
    cbm_db_lease_release(lease);
    /* A second, free database: the preview judges it without creating its
     * lock files (the lease's or the reader gate's). */
    char db2[800];
    ce_make_db(&e, "fresh", root, db2, sizeof(db2));
    ASSERT_TRUE(cbm_cache_usage_set(db2, now - 100 * CE_DAY, NULL));
    ASSERT_EQ(ce_sweep(&cfg, now, true, &r), 0);
    ASSERT_EQ(r.projects, 2); /* both free now: would be removed */
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(db2));
    ASSERT_FALSE(cbm_file_exists((std::string(db2) + ".index.lock").c_str()));
    ASSERT_FALSE(cbm_file_exists((std::string(db2) + ".read.lock").c_str()));
    /* No cache directory: the settings load does not create one. */
    char nocache[700];
    snprintf(nocache, sizeof(nocache), "%s/never-made", e.base);
    cbm_setenv("CBM_CACHE_DIR", nocache, 1);
    cbm_cache_expiry_cfg_load(&cfg, NULL);
    ASSERT_FALSE(cbm_is_dir(nocache));
    cbm_setenv("CBM_CACHE_DIR", e.cache, 1);
    ce_teardown(&e);
    PASS();
}

/* A <name>.db that is a symlink to a database outside the cache is not a
 * project of this cache: the sweep neither opens nor expires it, and the
 * file it points at is untouched. A slot file that is a symlink is never
 * restored. */
TEST(cache_linked_db_and_slot_are_ignored) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_lndb"));
    char root[700];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    /* The "outside" database: a real one, made in the cache then moved out. */
    char ext[800];
    ce_make_db(&e, "ext", root, ext, sizeof(ext));
    char outside[800];
    snprintf(outside, sizeof(outside), "%s/outside.db", e.base);
    ASSERT_EQ(cbm_rename_replace(ext, outside), 0);
    cbm_remove_db_sidecars(ext);
    int64_t size_before = (int64_t)cbm_file_size(outside);
    char link[800];
    snprintf(link, sizeof(link), "%s/linked.db", e.cache);
    ASSERT_EQ(symlink(outside, link), 0);
    ASSERT_TRUE(cbm_cache_usage_set(link, now - 200 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_TRUE(cbm_is_link(link));
    ASSERT_TRUE(cbm_file_exists(outside));
    ASSERT_EQ((int64_t)cbm_file_size(outside), size_before);
    ASSERT_EQ(ce_trash_count(&e), 0);
    /* A slot that is a link to that database is not an incoming slot. */
    char db[800];
    ce_make_db(&e, "p", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "p", root, sdir, sizeof(sdir)));
    ASSERT_TRUE(cbm_mkdir_p(sdir, 0755));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/feature.db", sdir);
    ASSERT_EQ(symlink(outside, slot), 0);
    char *from = NULL;
    ASSERT_EQ(cbm_branch_slots_switch(db, "p", "feature", root, 8, 0, &from),
              CBM_BRANCH_SWITCH_NONE);
    ASSERT_NULL(from);
    ASSERT_EQ((int64_t)cbm_file_size(outside), size_before);
    (void)cbm_unlink(slot);
    (void)cbm_unlink(link);
    ce_teardown(&e);
#endif
    PASS();
}

/* A project being indexed (its lease held) is skipped, and expired by a
 * later sweep once the lease is free. */
TEST(cache_skips_leased_project) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_lease"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "busy", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(db, &lease), 0);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_TRUE(cbm_file_exists(db));
    cbm_db_lease_release(lease);
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ce_teardown(&e);
    PASS();
}

/* A reader holding a snapshot that pins WAL frames keeps the database. */
TEST(cache_skips_database_with_open_reader) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_reader"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "read", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    sqlite3 *w = NULL;
    sqlite3 *rd = NULL;
    sqlite3_open(db, &w);
    sqlite3_exec(w, "PRAGMA wal_autocheckpoint=0; CREATE TABLE t(x); INSERT INTO t VALUES(1);",
                 NULL, NULL, NULL);
    sqlite3_open(db, &rd);
    sqlite3_exec(rd, "BEGIN; SELECT count(*) FROM t;", NULL, NULL, NULL);
    sqlite3_exec(w, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_TRUE(cbm_file_exists(db));
    sqlite3_exec(rd, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(rd);
    sqlite3_close(w);
    ce_teardown(&e);
    PASS();
}

/* A dry run reports what would go, with its size, and changes nothing. */
static void ce_count_report(const cbm_cache_expiry_item_t *it, void *ud) {
    if (it->kind == CBM_EXPIRE_UNUSED && !it->removed && !it->skipped && it->bytes > 0) {
        (*(int *)ud)++;
    }
}

TEST(cache_dry_run_changes_nothing) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_dry"));
    char root[700];
    char db[800];
    char gone[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "old", root, db, sizeof(db));
    ce_make_db(&e, "unseen", "/x-cbm-nowhere/r", gone, sizeof(gone));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.dry_run = true;
    int seen = 0;
    o.report = ce_count_report;
    o.report_ud = &seen;
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_EQ(seen, 1);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "unseen", &lu, &checks);
    ASSERT_EQ(lu, -1); /* no state written */
    ASSERT_TRUE(cbm_cache_expiry_due(&cfg, now)); /* and no sweep recorded */
    ce_teardown(&e);
    PASS();
}

/* Whether _config.db of the test cache has table `name`. */
static bool ce_config_has_table(const ce_env_t *e, const char *name) {
    char p[700];
    snprintf(p, sizeof(p), "%s/_config.db", e->cache);
    sqlite3 *db = NULL;
    bool has = false;
    if (sqlite3_open_v2(p, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            has = sqlite3_step(st) == SQLITE_ROW;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return has;
}

/* A dry run reads the store_meta memo without the ordinary open's schema
 * upkeep: a v1 memo table is not dropped, the current one not created, and
 * no usage table appears. The real sweep does both. */
TEST(cache_dry_run_leaves_memo_schema) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_drymemo"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "memo", root, db, sizeof(db));
    char cfgp[700];
    snprintf(cfgp, sizeof(cfgp), "%s/_config.db", e.cache);
    sqlite3 *c = NULL;
    ASSERT_EQ(sqlite3_open(cfgp, &c), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(c,
                           "DROP TABLE IF EXISTS store_meta_v2; DROP TABLE IF EXISTS cache_usage;"
                           "CREATE TABLE store_meta(db_name TEXT);",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(c);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), true, &r), 0);
    ASSERT_TRUE(ce_config_has_table(&e, "store_meta"));
    ASSERT_FALSE(ce_config_has_table(&e, "store_meta_v2"));
    ASSERT_FALSE(ce_config_has_table(&e, "cache_usage"));
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    ASSERT_FALSE(ce_config_has_table(&e, "store_meta"));
    ASSERT_TRUE(ce_config_has_table(&e, "store_meta_v2"));
    ASSERT_TRUE(ce_config_has_table(&e, "cache_usage"));
    ce_teardown(&e);
    PASS();
}

/* A project due for removal takes its parked slots with it: the preview
 * counts them once, in the project's bytes, as the real sweep does -- not
 * again as old slots of their own. */
TEST(cache_dry_run_counts_project_slots_once) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_dryslots"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "old", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 200 * CE_DAY, NULL));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "old", root, sdir, sizeof(sdir)));
    ASSERT_TRUE(cbm_mkdir_p(sdir, 0755));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/feature.db.zst", sdir);
    ASSERT_EQ(th_write_file(slot, "a parked slot, aged past the slot expiry"), 0);
    ce_age(slot, now - 60 * CE_DAY);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t dry;
    ASSERT_EQ(ce_sweep(&cfg, now, true, &dry), 0);
    ASSERT_EQ(dry.projects, 1);
    ASSERT_EQ(dry.slots, 0);
    cbm_cache_sweep_result_t real;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &real), 0);
    ASSERT_EQ(real.projects, 1);
    ASSERT_EQ(real.slots, 0);
    ASSERT_EQ(dry.bytes, real.bytes);
    ce_teardown(&e);
#endif
    PASS();
}

/* A dry run previews a missing root past its streak and grace as due, as
 * the real sweep then removes it -- not as "used since the scan" because
 * the preview's streak increment was never written. */
static void ce_count_root_gone(const cbm_cache_expiry_item_t *it, void *ud) {
    if (it->kind == CBM_EXPIRE_ROOT_GONE && !it->skipped) {
        (*(int *)ud)++;
    }
}

TEST(cache_dry_run_reports_root_gone_due) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_drygone"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "gone", root, db, sizeof(db));
    th_rmtree(root);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    int64_t t0 = (int64_t)time(NULL);
    for (int day = 0; day < 3; day++) {
        ASSERT_EQ(ce_sweep(&cfg, t0 + day * CE_DAY, false, &r), 0);
    }
    cbm_cache_sweep_opts_t o = {};
    o.now = t0 + 8 * CE_DAY;
    o.dry_run = true;
    int due = 0;
    o.report = ce_count_root_gone;
    o.report_ud = &due;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_EQ(r.skipped, 0);
    ASSERT_EQ(due, 1);
    ASSERT_TRUE(cbm_file_exists(db));
    int64_t lu;
    int checks;
    ce_usage(&e, "gone", &lu, &checks);
    ASSERT_EQ(checks, 3); /* the preview wrote no streak */
    ASSERT_EQ(ce_sweep(&cfg, t0 + 8 * CE_DAY, false, &r), 0);
    ASSERT_EQ(r.projects, 1); /* what the preview said */
    ASSERT_FALSE(cbm_file_exists(db));
    ce_teardown(&e);
    PASS();
}

/* Trash: an expired database with its parked slots can be restored; the
 * next sweep a day later purges what is left in the trash. */
TEST(cache_trash_restore_and_purge) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_trash"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "tp", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "tp", root, sdir, sizeof(sdir)));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/feature.db.zst", sdir);
    ASSERT_EQ(th_write_file(slot, "zstd"), 0);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_FALSE(cbm_file_exists(slot));

    char msg[512];
    ASSERT_EQ(cbm_cache_trash_restore("tp", msg, sizeof(msg)), 0);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(slot));
    ASSERT_EQ(ce_trash_count(&e), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "tp", &lu, &checks);
    ASSERT_GTE(lu, now); /* restored: the clock restarts */
    /* A second restore finds nothing; a live database blocks one. */
    ASSERT_NEQ(cbm_cache_trash_restore("tp", msg, sizeof(msg)), 0);

    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(ce_trash_count(&e), 1);
    ASSERT_EQ(ce_sweep(&cfg, now + 3600, false, &r), 0); /* too soon */
    ASSERT_EQ(r.purged, 0);
    ASSERT_EQ(ce_trash_count(&e), 1);
    ASSERT_EQ(ce_sweep(&cfg, now + 2 * CE_DAY, false, &r), 0);
    ASSERT_EQ(r.purged, 1);
    ASSERT_EQ(ce_trash_count(&e), 0);
    ce_teardown(&e);
    PASS();
}

/* A purge waits out a restore of the same project: cache restore holds the
 * project's lease while it moves the database, its WAL and its slots back,
 * and a purge that deleted the entry in between would leave half of it. */
TEST(cache_trash_purge_skips_leased_project) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_trlease"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "rs", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_EQ(ce_trash_count(&e), 1);

    cbm_db_lease_t *lease = NULL; /* what cache restore holds */
    ASSERT_EQ(cbm_db_lease_try_acquire(db, &lease), 0);
    ASSERT_EQ(ce_sweep(&cfg, now + 2 * CE_DAY, false, &r), 0);
    ASSERT_EQ(r.purged, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_EQ(ce_trash_count(&e), 1);
    cbm_db_lease_release(lease);

    ASSERT_EQ(ce_sweep(&cfg, now + 2 * CE_DAY, false, &r), 0);
    ASSERT_EQ(r.purged, 1);
    ASSERT_EQ(ce_trash_count(&e), 0);
    ce_teardown(&e);
    PASS();
}

#ifndef _WIN32
/* Purging a trash entry removes a link inside it and never follows it: a
 * link to a directory outside the cache keeps that directory's files, a
 * cyclic link ends the walk, and the size counts none of it. A trash entry
 * that is itself a link is never listed or followed. On Windows the same
 * walk meets a directory junction, which cbm_readdir calls a directory. */
TEST(cache_trash_purge_never_follows_links) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_trlink"));
    int64_t now = (int64_t)time(NULL);
    char outside[700];
    char keep[800];
    snprintf(outside, sizeof(outside), "%s/outside", e.base);
    ASSERT_TRUE(cbm_mkdir_p(outside, 0755));
    snprintf(keep, sizeof(keep), "%s/keep.txt", outside);
    std::string big(1 << 20, 'x');
    ASSERT_EQ(th_write_file(keep, big.c_str()), 0);

    char entry[800];
    snprintf(entry, sizeof(entry), "%s/trash/%lld-lp", e.cache, (long long)(now - 10 * CE_DAY));
    char sub[900];
    snprintf(sub, sizeof(sub), "%s/branches", entry);
    ASSERT_TRUE(cbm_mkdir_p(sub, 0755));
    char f[1000];
    snprintf(f, sizeof(f), "%s/lp.db", entry);
    ASSERT_EQ(th_write_file(f, "db"), 0);
    char lnk[1000];
    snprintf(lnk, sizeof(lnk), "%s/outside", sub);
    ASSERT_EQ(symlink(outside, lnk), 0);
    char loop[1000];
    snprintf(loop, sizeof(loop), "%s/loop", sub);
    ASSERT_EQ(symlink(entry, loop), 0);
    ASSERT_TRUE(cbm_is_link(lnk));
    ASSERT_TRUE(cbm_is_dir(lnk)); /* what the walk must not trust */
    ASSERT_FALSE(cbm_is_link(sub));
    ASSERT_FALSE(cbm_is_link(f));

    /* An entry that is a link itself, to the outside directory. */
    char lentry[800];
    snprintf(lentry, sizeof(lentry), "%s/trash/%lld-ln", e.cache, (long long)(now - 10 * CE_DAY));
    ASSERT_EQ(symlink(outside, lentry), 0);
    char msg[512];
    ASSERT_NEQ(cbm_cache_trash_restore("ln", msg, sizeof(msg)), 0);

    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.purged, 1);
    ASSERT_LT(r.bytes, (int64_t)big.size());
    ASSERT_FALSE(cbm_file_exists(entry));
    ASSERT_TRUE(cbm_file_exists(keep));
    ASSERT_TRUE(cbm_is_link(lentry));
    (void)cbm_unlink(lentry);
    ce_teardown(&e);
    PASS();
}

/* <cache>/trash standing as a link to a directory outside the cache is never
 * followed: nothing in there is purged, and nothing is moved through it. */
TEST(cache_trash_root_link_is_refused) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_trroot"));
    int64_t now = (int64_t)time(NULL);
    char outside[700];
    snprintf(outside, sizeof(outside), "%s/outside", e.base);
    char victim_dir[800];
    snprintf(victim_dir, sizeof(victim_dir), "%s/%lld-x", outside, (long long)(now - 10 * CE_DAY));
    ASSERT_TRUE(cbm_mkdir_p(victim_dir, 0755));
    char victim[900];
    snprintf(victim, sizeof(victim), "%s/x.db", victim_dir);
    ASSERT_EQ(th_write_file(victim, "keep"), 0);
    char tdir[700];
    snprintf(tdir, sizeof(tdir), "%s/trash", e.cache);
    ASSERT_EQ(symlink(outside, tdir), 0);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.purged, 0);
    ASSERT_TRUE(cbm_file_exists(victim));
    ASSERT_TRUE(cbm_is_link(tdir));
    /* A removal that would move a database through the link is refused. */
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "mv", root, db, sizeof(db));
    char entry[900];
    snprintf(entry, sizeof(entry), "%s/%lld-mv", tdir, (long long)now);
    int parked = 0;
    ASSERT_EQ(cbm_cache_remove_project(db, "mv", entry, &parked), -1);
    ASSERT_TRUE(cbm_file_exists(db));
    char moved[1000];
    snprintf(moved, sizeof(moved), "%s/%lld-mv/mv.db", outside, (long long)now);
    ASSERT_FALSE(cbm_file_exists(moved));
    (void)cbm_unlink(tdir);
    ce_teardown(&e);
#endif
    PASS();
}

/* <cache>/branches (or a project's directory under it) standing as a link
 * is never scanned for deletion: old slots behind it stay. */
TEST(cache_slots_dir_link_is_refused) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_sllink"));
    int64_t now = (int64_t)time(NULL);
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "ln", root, db, sizeof(db));
    char outside[700];
    snprintf(outside, sizeof(outside), "%s/outside/ln", e.base);
    ASSERT_TRUE(cbm_mkdir_p(outside, 0755));
    char victim[900];
    snprintf(victim, sizeof(victim), "%s/old.db.zst", outside);
    ASSERT_EQ(th_write_file(victim, "slot"), 0);
    ce_age(victim, now - 60 * CE_DAY);
    char bdir[700];
    snprintf(bdir, sizeof(bdir), "%s/branches", e.cache);
    char parent[700];
    snprintf(parent, sizeof(parent), "%s/outside", e.base);
    ASSERT_EQ(symlink(parent, bdir), 0);
    int reported = 0;
    ASSERT_EQ(cbm_branch_slots_expire(db, "ln", now - 30 * CE_DAY, now - 3600, false,
                                      ce_count_slot, &reported),
              0);
    ASSERT_EQ(reported, 0);
    ASSERT_EQ(cbm_branch_slots_delete_all(db, "ln"), 0);
    ASSERT_TRUE(cbm_file_exists(victim));
    /* A switch neither parks into nor restores from the linked directory. */
    char *from = NULL;
    ASSERT_EQ(cbm_branch_slots_switch(db, "ln", "feature", root, 8, 0, &from),
              CBM_BRANCH_SWITCH_NONE);
    ASSERT_NULL(from);
    ASSERT_TRUE(cbm_file_exists(victim));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.slots, 0);
    ASSERT_TRUE(cbm_file_exists(victim));
    (void)cbm_unlink(bdir);
    /* The project directory itself as a link, under a real branches dir. */
    ASSERT_TRUE(cbm_mkdir_p(bdir, 0755));
    char pdir[800];
    snprintf(pdir, sizeof(pdir), "%s/ln", bdir);
    ASSERT_EQ(symlink(outside, pdir), 0);
    ASSERT_EQ(cbm_branch_slots_delete_all(db, "ln"), 0);
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.slots, 0);
    ASSERT_TRUE(cbm_file_exists(victim));
    (void)cbm_unlink(pdir);
    ce_teardown(&e);
#endif
    PASS();
}

/* A removal whose parked slot refuses to go (a read-only slot directory)
 * reports that (rc 2) rather than success: a kept slot could later restore
 * graph data the caller was told is gone. */
TEST(cache_remove_project_reports_slots_left) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root ignores directory permissions */
    }
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rmleft"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "rl", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "rl", root, sdir, sizeof(sdir)));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/main.db.zst", sdir);
    th_write_file(slot, "a");
    ASSERT_EQ(chmod(sdir, 0555), 0);
    int parked = -1;
    int rc = cbm_cache_remove_project(db, "rl", NULL, &parked);
    chmod(sdir, 0755);
    ASSERT_EQ(rc, 2);
    ASSERT_EQ(parked, 0);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(slot));
    ce_teardown(&e);
#endif
    PASS();
}

/* A trash entry with a file that refuses to go (a read-only directory) is
 * skipped, not reported purged, and purged by a later sweep once it can be. */
TEST(cache_trash_purge_refused_is_skipped) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root ignores directory permissions */
    }
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_trro"));
    int64_t now = (int64_t)time(NULL);
    char entry[800];
    snprintf(entry, sizeof(entry), "%s/trash/%lld-ro", e.cache, (long long)(now - 10 * CE_DAY));
    char sub[900];
    snprintf(sub, sizeof(sub), "%s/branches", entry);
    ASSERT_TRUE(cbm_mkdir_p(sub, 0755));
    char f[1000];
    snprintf(f, sizeof(f), "%s/main.db.zst", sub);
    ASSERT_EQ(th_write_file(f, "slot"), 0);
    ASSERT_EQ(chmod(sub, 0555), 0);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.purged, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_EQ(r.bytes, 0);
    ASSERT_TRUE(cbm_file_exists(f));
    ASSERT_EQ(ce_trash_count(&e), 1);
    chmod(sub, 0755);
    ASSERT_EQ(ce_sweep(&cfg, now + CE_DAY, false, &r), 0);
    ASSERT_EQ(r.purged, 1);
    ASSERT_FALSE(cbm_file_exists(entry));
    ASSERT_EQ(ce_trash_count(&e), 0);
    ce_teardown(&e);
#endif
    PASS();
}

/* A parked slot older than the slot age goes (database or not); a newer one
 * stays; old scratch of an interrupted park goes. */
TEST(cache_old_slots_expire) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_slots"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "sp", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "sp", root, sdir, sizeof(sdir)));
    char old_slot[1300];
    char new_slot[1300];
    char legacy[1300];
    char scratch[1300];
    snprintf(old_slot, sizeof(old_slot), "%s/deleted-branch.db.zst", sdir);
    snprintf(new_slot, sizeof(new_slot), "%s/main.db.zst", sdir);
    snprintf(legacy, sizeof(legacy), "%s/older.db", sdir);
    snprintf(scratch, sizeof(scratch), "%s/x.db.unz", sdir);
    th_write_file(old_slot, "a");
    th_write_file(new_slot, "b");
    th_write_file(legacy, "c");
    th_write_file(scratch, "d");
    ce_age(old_slot, now - 31 * CE_DAY);
    ce_age(legacy, now - 45 * CE_DAY);
    ce_age(new_slot, now - 29 * CE_DAY);
    ce_age(scratch, now - 2 * 3600);
    /* An orphan project directory (its database already gone). */
    char odb[800];
    snprintf(odb, sizeof(odb), "%s/orphan.db", e.cache);
    char odir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(odb, "orphan", root, odir, sizeof(odir)));
    char oslot[1300];
    snprintf(oslot, sizeof(oslot), "%s/main.db.zst", odir);
    th_write_file(oslot, "e");
    ce_age(oslot, now - 60 * CE_DAY);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.slots, 3);
    ASSERT_EQ(r.sidecars, 1);
    ASSERT_FALSE(cbm_file_exists(old_slot));
    ASSERT_FALSE(cbm_file_exists(legacy));
    ASSERT_FALSE(cbm_file_exists(scratch));
    ASSERT_FALSE(cbm_file_exists(oslot));
    ASSERT_FALSE(cbm_file_exists(odir));
    ASSERT_TRUE(cbm_file_exists(new_slot));
    ASSERT_TRUE(cbm_file_exists(db));
    ce_teardown(&e);
    PASS();
}

/* SQLite sidecars of a legacy .db slot stay while the slot does, however
 * old; once the slot is gone (an interrupted delete) they are scratch and
 * expire by the scratch age. */
TEST(cache_orphan_slot_sidecars_expire) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_slotside"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "ss", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "ss", root, sdir, sizeof(sdir)));
    const char *gone[] = {"lost.db-wal", "lost.db-shm", "lost.db-journal"};
    const char *stay[] = {"kept.db", "kept.db-wal", "kept.db-shm", "fresh.db-wal"};
    char p[1300];
    for (const char *n : gone) {
        snprintf(p, sizeof(p), "%s/%s", sdir, n);
        ASSERT_EQ(th_write_file(p, "x"), 0);
        ce_age(p, now - 2 * 3600);
    }
    for (const char *n : stay) {
        snprintf(p, sizeof(p), "%s/%s", sdir, n);
        ASSERT_EQ(th_write_file(p, "x"), 0);
        ce_age(p, strcmp(n, "fresh.db-wal") == 0 ? now - 60
                  : strcmp(n, "kept.db") == 0    ? now - CE_DAY
                                                 : now - 20 * CE_DAY);
    }
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.slots, 0);
    ASSERT_EQ(r.sidecars, 3);
    for (const char *n : gone) {
        snprintf(p, sizeof(p), "%s/%s", sdir, n);
        ASSERT_FALSE(cbm_file_exists(p));
    }
    for (const char *n : stay) {
        snprintf(p, sizeof(p), "%s/%s", sdir, n);
        ASSERT_TRUE(cbm_file_exists(p));
    }
    ce_teardown(&e);
    PASS();
}

/* Orphan sidecars and temp files go; those of a live database, fresh ones,
 * and every .index.lock stay. */
TEST(cache_orphan_sidecars) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_side"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "live", root, db, sizeof(db));
    const char *gone[] = {"gone.db-wal",     "gone.db-shm",        "gone.db.tmp",
                          "gone.db.tmp-wal", "gone.db.import_tmp", "gone.db.import_tmp-shm"};
    const char *stay[] = {"live.db-wal", "gone.db.index.lock", "fresh.db-wal", "notes.txt"};
    char p[900];
    for (const char *n : gone) {
        snprintf(p, sizeof(p), "%s/%s", e.cache, n);
        th_write_file(p, "x");
        ce_age(p, now - 2 * 3600);
    }
    for (const char *n : stay) {
        snprintf(p, sizeof(p), "%s/%s", e.cache, n);
        th_write_file(p, "x");
        ce_age(p, strcmp(n, "fresh.db-wal") == 0 ? now - 60 : now - 30 * CE_DAY);
    }
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.sidecars, 6);
    for (const char *n : gone) {
        snprintf(p, sizeof(p), "%s/%s", e.cache, n);
        ASSERT_FALSE(cbm_file_exists(p));
    }
    for (const char *n : stay) {
        snprintf(p, sizeof(p), "%s/%s", e.cache, n);
        ASSERT_TRUE(cbm_file_exists(p));
    }
    ce_teardown(&e);
    PASS();
}
#endif

/* Usage is written at most once an hour per database; an index run (with
 * its root) always writes and clears a missing streak. */
TEST(cache_usage_touch_is_throttled) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_touch"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "touchy", root, db, sizeof(db));
    int64_t now = (int64_t)time(NULL);
    int64_t lu;
    int checks;
    cbm_cache_usage_touch(db, NULL);
    ce_usage(&e, "touchy", &lu, &checks);
    ASSERT_GTE(lu, now);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 500, NULL));
    cbm_cache_usage_touch(db, NULL); /* within the hour: nothing written */
    ce_usage(&e, "touchy", &lu, &checks);
    ASSERT_EQ(lu, now - 500);
    cbm_cache_usage_touch(db, root);
    ce_usage(&e, "touchy", &lu, &checks);
    ASSERT_GTE(lu, now);
    ce_teardown(&e);
    PASS();
}

/* A touch that could not write (_config.db held by another writer, as a
 * sweep's transaction holds it) is not throttled: the next touch writes. */
TEST(cache_usage_touch_retries_after_busy) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_touchbusy"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "touchbusy", root, db, sizeof(db));
    int64_t now = (int64_t)time(NULL);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 5000, NULL));
    char cp[700];
    snprintf(cp, sizeof(cp), "%s/_config.db", e.cache);
    sqlite3 *w = NULL;
    ASSERT_EQ(sqlite3_open(cp, &w), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(w, "BEGIN IMMEDIATE;", NULL, NULL, NULL), SQLITE_OK);
    cbm_cache_usage_touch(db, NULL); /* busy: nothing written */
    sqlite3_exec(w, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(w);
    int64_t lu;
    int checks;
    ce_usage(&e, "touchbusy", &lu, &checks);
    ASSERT_EQ(lu, now - 5000);
    cbm_cache_usage_touch(db, NULL);
    ce_usage(&e, "touchbusy", &lu, &checks);
    ASSERT_GTE(lu, now);
    ce_teardown(&e);
    PASS();
}

/* Many index runs recording their use at once, while another connection
 * keeps taking _config.db's lock (300 ms on, 300 ms off) for longer than the
 * sweep's reads wait:
 * every row lands. (In rollback mode an exclusive holder locks readers out,
 * and the open's table check gave up after 200 ms: rows were lost.) The
 * file ends up in WAL mode. */
TEST(cache_usage_concurrent_writers_all_recorded) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_usagerace"));
    char cp[700];
    snprintf(cp, sizeof(cp), "%s/_config.db", e.cache);
    enum { THREADS = 12, PER = 6, HOLDS = 10 };
    std::atomic<bool> holding{false};
    std::thread holder([&] {
        sqlite3 *h = NULL;
        if (sqlite3_open(cp, &h) != SQLITE_OK) {
            return;
        }
        sqlite3_busy_timeout(h, 5000);
        for (int i = 0; i < HOLDS; i++) {
            if (sqlite3_exec(h, "BEGIN EXCLUSIVE;", NULL, NULL, NULL) == SQLITE_OK) {
                holding = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                sqlite3_exec(h, "COMMIT;", NULL, NULL, NULL);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        sqlite3_close(h);
    });
    for (int i = 0; i < 200 && !holding; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::atomic<int> failed{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < THREADS; t++) {
        writers.emplace_back([&, t] {
            for (int k = 0; k < PER; k++) {
                char p[900];
                snprintf(p, sizeof(p), "%s/w%02d_%d.db", e.cache, t, k);
                char root[900];
                snprintf(root, sizeof(root), "%s/root%02d_%d", e.base, t, k);
                if (k % 2 == 0) {
                    if (!cbm_cache_usage_set(p, (int64_t)time(NULL), root)) {
                        failed++;
                    }
                } else {
                    cbm_cache_usage_touch(p, root);
                }
                cbm_store_meta_close(cbm_store_meta_open());
            }
        });
    }
    for (auto &w : writers) {
        w.join();
    }
    holder.join();
    ASSERT_EQ(failed.load(), 0);
    /* With the holder gone, the next writer's open converts at the latest. */
    char last[900];
    snprintf(last, sizeof(last), "%s/after.db", e.cache);
    ASSERT_TRUE(cbm_cache_usage_set(last, (int64_t)time(NULL), NULL));
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open(cp, &db), SQLITE_OK);
    sqlite3_stmt *st = NULL;
    int rows = -1;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM cache_usage WHERE db_name LIKE 'w%';", -1,
                           &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        rows = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    char mode[16] = "";
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        snprintf(mode, sizeof(mode), "%s", (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    ASSERT_EQ(rows, THREADS * PER);
    ASSERT_STR_EQ(mode, "wal");
    ce_teardown(&e);
    PASS();
}

/* A use recorded between the sweep's scan and the move (another process's
 * tool call; the keep callback runs in that window) keeps the database. */
static bool ce_touch_between(const char *project, void *ud) {
    (void)project;
    (void)cbm_cache_usage_set((const char *)ud, (int64_t)time(NULL), NULL);
    return false;
}

TEST(cache_used_since_scan_is_kept) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_race"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "raced", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 200 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.keep = ce_touch_between;
    o.keep_ud = db;
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    ce_teardown(&e);
    PASS();
}

/* A use recorded while the sweep scans (learning roots, statting them) lands
 * and keeps the database: the scan holds no write lock on _config.db, and
 * the rows it writes afterwards never overwrite a use recorded meanwhile.
 * Two databases, so the scan has rows to write: a fresh one and an idle one
 * whose root is not yet recorded. */
static void ce_touch_during_scan(void *ud) {
    cbm_cache_usage_touch((const char *)ud, NULL);
}

TEST(cache_touch_during_scan_lands) {
#ifdef CBM_ENABLE_TEST_SEAMS
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_scantouch"));
    char root[700];
    char db[800];
    char fresh[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "scantouched", root, db, sizeof(db));
    ce_make_db(&e, "scanfresh", root, fresh, sizeof(fresh));
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 200 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_expiry_test_scan_hook(ce_touch_during_scan, db);
    cbm_cache_sweep_result_t r;
    int rc = ce_sweep(&cfg, now, false, &r);
    cbm_cache_expiry_test_scan_hook(NULL, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "scantouched", &lu, &checks);
    ASSERT_GTE(lu, now);
    ce_usage(&e, "scanfresh", &lu, &checks);
    ASSERT_EQ(lu, now); /* the fresh database's row was still written */
    ce_teardown(&e);
#endif
    PASS();
}

/* End to end through the server: an index run and a tool call record use;
 * delete_project removes the usage row with the database. */
TEST(cache_index_and_query_record_use) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_e2e"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    char *name = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(name);
    int64_t lu;
    int checks;
    ce_usage(&e, name, &lu, &checks);
    ASSERT_GT(lu, 0);
    ASSERT_EQ(checks, 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", name);
    char *resp = cbm_mcp_handle_tool(srv, "index_status", args);
    free(resp);
    resp = cbm_mcp_handle_tool(srv, "delete_project", args);
    ASSERT_NOT_NULL(strstr(resp, "deleted"));
    free(resp);
    cbm_mcp_server_free(srv);
    ce_usage(&e, name, &lu, &checks);
    ASSERT_EQ(lu, -1);
    free(name);
    ce_teardown(&e);
    PASS();
}

/* The session auto-index outside a supervisor host (and with the supervisor
 * switched off) runs the pipeline in process: that run is a use too, and it
 * records the checkout root, like every other index path. */
TEST(cache_autoindex_in_process_records_use) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_auto"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    char *real = realpath(repo, NULL);
    ASSERT_NOT_NULL(real);
    char old_cwd[1024];
    ASSERT_NOT_NULL(getcwd(old_cwd, sizeof(old_cwd)));
    char old_expiry[64];
    char old_update[64];
    bool had_expiry = cbm_safe_getenv("CBM_CACHE_EXPIRY", old_expiry, sizeof(old_expiry), NULL);
    bool had_update = cbm_safe_getenv("CBM_UPDATE_CHECK", old_update, sizeof(old_update), NULL);
    cbm_setenv("CBM_CACHE_EXPIRY", "0", 1); /* no sweep: it would record the row itself */
    cbm_setenv("CBM_UPDATE_CHECK", "0", 1);
    cbm_config_t *cfgs = cbm_config_open(e.cache);
    ASSERT_NOT_NULL(cfgs);
    cbm_config_set(cfgs, CBM_CONFIG_AUTO_INDEX, "true");
    ASSERT_EQ(chdir(real), 0);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_config(srv, cfgs);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
             "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
             "\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}");
    free(resp);
    cbm_mcp_server_join_threads(srv);
    cbm_mcp_server_free(srv);
    (void)chdir(old_cwd);
    cbm_config_close(cfgs);
    if (had_expiry) {
        cbm_setenv("CBM_CACHE_EXPIRY", old_expiry, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_EXPIRY");
    }
    if (had_update) {
        cbm_setenv("CBM_UPDATE_CHECK", old_update, 1);
    } else {
        cbm_unsetenv("CBM_UPDATE_CHECK");
    }
    char *name = cbm_project_name_from_path(real);
    ASSERT_NOT_NULL(name);
    char db[900];
    snprintf(db, sizeof(db), "%s/%s.db", e.cache, name);
    ASSERT_TRUE(cbm_file_exists(db)); /* the auto-index ran */
    int64_t lu;
    int checks;
    ce_usage(&e, name, &lu, &checks);
    ASSERT_GT(lu, 0);
    ASSERT_EQ(checks, 0);
    free(name);
    free(real);
    ce_teardown(&e);
#endif
    PASS();
}

typedef struct {
    const ce_env_t *e;
    int calls;
    int lease_rc;
} ce_indexed_probe_t;

static void ce_on_indexed(const cbm_pipeline_t *p, void *ud) {
    ce_indexed_probe_t *probe = (ce_indexed_probe_t *)ud;
    probe->calls++;
    char db[800];
    snprintf(db, sizeof(db), "%s/%s.db", probe->e->cache, cbm_pipeline_project_name(p));
    cbm_db_lease_t *lease = NULL;
    probe->lease_rc = cbm_db_lease_try_acquire(db, &lease);
    cbm_db_lease_release(lease);
}

/* The pipeline's on-indexed callback runs once per successful run, while
 * the run still holds the database's index lease: what it records, a sweep
 * in another process can only judge after it. */
TEST(cache_pipeline_on_indexed_holds_lease) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_onidx"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ce_indexed_probe_t probe = {&e, 0, 0};
    cbm_pipeline_set_on_indexed(p, ce_on_indexed, &probe);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    ASSERT_EQ(probe.calls, 1);
    ASSERT_EQ(probe.lease_rc, CBM_INDEX_BUSY);
    cbm_pipeline_free(p);
    ce_teardown(&e);
    PASS();
}

/* A run whose requested artifact export fails after the database was
 * committed still fails, but it did build the database: it records the use,
 * or the sweep the request handler starts next could trash the fresh
 * database by its old last_used. */
TEST(cache_pipeline_on_indexed_after_failed_export) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_onidx_exp"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    /* A file where the artifact directory goes: the export cannot create it. */
    snprintf(f, sizeof(f), "%s/.code-cortex", repo);
    ASSERT_EQ(th_write_file(f, "not a directory\n"), 0);
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    ce_indexed_probe_t probe = {&e, 0, 0};
    cbm_pipeline_set_on_indexed(p, ce_on_indexed, &probe);
    ASSERT_NEQ(cbm_pipeline_run(p), 0);
    char db[800];
    snprintf(db, sizeof(db), "%s/%s.db", e.cache, cbm_pipeline_project_name(p));
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(probe.calls, 1);
    ASSERT_EQ(probe.lease_rc, CBM_INDEX_BUSY);
    cbm_pipeline_free(p);
    ce_teardown(&e);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
typedef struct {
    int projects;
    int runs;
} ce_gap_sweep_t;

/* A sweep in the moment after the index run released its lease. */
static void ce_sweep_in_gap(const char *project, void *data) {
    (void)project;
    ce_gap_sweep_t *g = (ce_gap_sweep_t *)data;
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    if (ce_sweep(&cfg, (int64_t)time(NULL), false, &r) == 0) {
        g->projects += r.projects;
        g->runs++;
    }
}
#endif

/* An index run records its use before it releases the index lease. A sweep
 * that takes the lease the moment the run lets go of it (one in another
 * process that judged the database stale before the run) re-reads the usage
 * row and finds the run's use there, so it keeps the database it just built. */
TEST(cache_index_use_recorded_before_lease_release) {
#ifdef CBM_ENABLE_TEST_SEAMS
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_idxgap"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    char *name = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(name);
    char db[800];
    snprintf(db, sizeof(db), "%s/%s.db", e.cache, name);
    ASSERT_TRUE(cbm_file_exists(db));
    int64_t now = (int64_t)time(NULL);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));

    ce_gap_sweep_t g = {0, 0};
    cbm_mcp_test_index_hook(ce_sweep_in_gap, &g);
    int rc = cbm_mcp_index_in_process(repo);
    cbm_mcp_test_index_hook(NULL, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(g.runs, 1);
    ASSERT_EQ(g.projects, 0);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, name, &lu, &checks);
    ASSERT_GTE(lu, now);
    free(name);
    ce_teardown(&e);
    PASS();
#else
    SKIP("requires CBM_TEST_SEAMS");
#endif
}

/* A file named otherwise than the project it holds (renamed, or a legacy
 * name the fallback scan still serves) is judged as that project: the
 * session's and the watched one are kept by their project names. */
TEST(cache_renamed_db_kept_by_project) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rnkeep"));
    char root[700];
    char a[800];
    char b[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db_as(&e, "old-sess", "sess", root, a, sizeof(a));
    ce_make_db_as(&e, "old-watched", "watched", root, b, sizeof(b));
    ASSERT_TRUE(cbm_cache_usage_set(a, now - 200 * CE_DAY, NULL));
    ASSERT_TRUE(cbm_cache_usage_set(b, now - 200 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.keep_project = "sess";
    o.keep = ce_keep_w;
    o.keep_ud = (void *)"watched";
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 2);
    ASSERT_TRUE(cbm_file_exists(a));
    ASSERT_TRUE(cbm_file_exists(b));
    ce_teardown(&e);
    PASS();
}

static void ce_name_report(const cbm_cache_expiry_item_t *it, void *ud) {
    if (it->kind == CBM_EXPIRE_UNUSED) {
        snprintf((char *)ud, 256, "%s", it->name);
    }
}

/* An expired renamed file: the dry run names the project and the file, its
 * parked slots (filed under the project) go to the trash with it, and the
 * restore puts it back as <project>.db, the name it is served under. */
TEST(cache_renamed_db_expires_as_project) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rntrash"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db_as(&e, "old-copy", "rp", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "rp", root, sdir, sizeof(sdir)));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/feature.db.zst", sdir);
    ASSERT_EQ(th_write_file(slot, "zstd"), 0);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.dry_run = true;
    char shown[256] = "";
    o.report = ce_name_report;
    o.report_ud = shown;
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_STR_EQ(shown, "rp (old-copy.db)");

    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_FALSE(cbm_file_exists(slot));
    ASSERT_EQ(ce_trash_count(&e), 1);

    char msg[512];
    ASSERT_EQ(cbm_cache_trash_restore("rp", msg, sizeof(msg)), 0);
    char back[800];
    snprintf(back, sizeof(back), "%s/rp.db", e.cache);
    ASSERT_TRUE(cbm_file_exists(back));
    ASSERT_TRUE(cbm_file_exists(slot));
    ASSERT_EQ(ce_trash_count(&e), 0);
    ce_teardown(&e);
    PASS();
}

/* A query that the fallback scan answers from a renamed file records use on
 * that file, so an actively used one is not "unused". */
TEST(cache_query_on_renamed_db_records_use) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rnuse"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    char *name = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(name);
    char from[900];
    char to[900];
    snprintf(from, sizeof(from), "%s/%s.db", e.cache, name);
    snprintf(to, sizeof(to), "%s/ce-renamed.db", e.cache);
    ASSERT_EQ(cbm_rename_replace(from, to), 0);
    int64_t now = (int64_t)time(NULL);
    ASSERT_TRUE(cbm_cache_usage_set(to, now - 100 * CE_DAY, NULL));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\"a\"}", name);
    char *resp = cbm_mcp_handle_tool(srv, "search_graph", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "not found"));
    free(resp);
    cbm_mcp_server_free(srv);
    int64_t lu;
    int checks;
    ce_usage(&e, "ce-renamed", &lu, &checks);
    ASSERT_GTE(lu, now);

    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, true, &r), 0);
    ASSERT_EQ(r.projects, 0);
    free(name);
    ce_teardown(&e);
    PASS();
}

static void ce_skip_report(const cbm_cache_expiry_item_t *it, void *ud) {
    if (it->kind == CBM_EXPIRE_UNUSED && it->skipped) {
        snprintf((char *)ud, 256, "%s", it->skipped);
    }
}

/* A server that holds a project's database open (its cached store, the
 * reader gate shared) keeps it from the sweep of another process, though
 * the usage row says unused and no read transaction is open: on POSIX the
 * move would leave that connection on the trashed file and the next call
 * without the project. Once the reader closes, the database goes. `file` is
 * the database's filename stem: the project's own, or a renamed file that
 * the fallback scan serves. */
static int ce_reader_gate_case(const char *prefix, bool renamed) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, prefix));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    char *name = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(name);
    char db[900];
    snprintf(db, sizeof(db), "%s/%s.db", e.cache, name);
    if (renamed) {
        char to[900];
        snprintf(to, sizeof(to), "%s/ce-gated.db", e.cache);
        ASSERT_EQ(cbm_rename_replace(db, to), 0);
        snprintf(db, sizeof(db), "%s", to);
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\"a\"}", name);
    char *resp = cbm_mcp_handle_tool(srv, "search_graph", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "not found"));
    free(resp);
    int64_t now = (int64_t)time(NULL);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 200 * CE_DAY, NULL));

    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    char why[256] = "";
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.report = ce_skip_report;
    o.report_ud = why;
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_STR_EQ(why, "in use by a reader");
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 0);
    /* The reader is still served from the file at its path. */
    resp = cbm_mcp_handle_tool(srv, "search_graph", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv); /* closes the cached store and its gate */
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 200 * CE_DAY, NULL));
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_EQ(ce_trash_count(&e), 1);
    free(name);
    ce_teardown(&e);
    PASS();
}

TEST(cache_open_reader_keeps_database) {
    return ce_reader_gate_case("cbm_ce_gate", false);
}

TEST(cache_open_reader_keeps_renamed_database) {
    return ce_reader_gate_case("cbm_ce_gatern", true);
}

/* A renamed file's parked slots are filed under its project, as are those of
 * the project's own <project>.db, which an index of the project writes under
 * that file's lease: while it is held, the renamed file and the slots stay. */
TEST(cache_renamed_db_waits_for_project_lease) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rnlease"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db_as(&e, "old-copy", "rp", root, db, sizeof(db));
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "rp", root, sdir, sizeof(sdir)));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/feature.db.zst", sdir);
    ASSERT_EQ(th_write_file(slot, "zstd"), 0);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    char canon[800];
    snprintf(canon, sizeof(canon), "%s/rp.db", e.cache);
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(canon, &lease), 0);

    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    char why[256] = "";
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.report = ce_skip_report;
    o.report_ud = why;
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(cbm_cache_expiry_sweep(&cfg, &o, &r), 0);
    ASSERT_EQ(r.projects, 0);
    ASSERT_EQ(r.skipped, 1);
    ASSERT_STR_EQ(why, "its project being indexed");
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(slot));

    cbm_db_lease_release(lease);
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_FALSE(cbm_file_exists(slot));
    ce_teardown(&e);
    PASS();
}

/* A legacy or renamed file whose filename is no valid project name ("old
 * project.db") is served by the fallback scan under its internal name; the
 * sweep tracks and expires it under that name too. A copy whose own name
 * would have to stand in (a second file claims the project) and is no safe
 * path component is not judged. */
TEST(cache_renamed_db_with_unsafe_stem_expires) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rnstem"));
    char root[700];
    char db[800];
    char dup[800];
    char twin[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db_as(&e, "old project", "oldproj", root, db, sizeof(db));
    ce_make_db_as(&e, "dup copy", "dupproj", root, dup, sizeof(dup));
    ce_make_db(&e, "dupproj", root, twin, sizeof(twin));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0); /* first seen: clocks start */
    int64_t lu;
    int checks;
    ce_usage(&e, "old project", &lu, &checks);
    ASSERT_EQ(lu, now);
    ce_usage(&e, "dup copy", &lu, &checks);
    ASSERT_EQ(lu, -1);

    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    ASSERT_TRUE(cbm_cache_usage_set(dup, now - 100 * CE_DAY, NULL));
    ASSERT_TRUE(cbm_cache_usage_set(twin, now - 1 * CE_DAY, NULL));
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.projects, 1);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(dup));
    ASSERT_TRUE(cbm_file_exists(twin));
    char msg[512];
    ASSERT_EQ(cbm_cache_trash_restore("oldproj", msg, sizeof(msg)), 0);
    char back[800];
    snprintf(back, sizeof(back), "%s/oldproj.db", e.cache);
    ASSERT_TRUE(cbm_file_exists(back));
    ce_teardown(&e);
    PASS();
}

/* A reader that finds the gate held exclusive past its wait (a sweep moving
 * the file) gets no connection, never an ungated one that the move would
 * leave on the trashed file. The fallback scan reports the lookup as
 * incomplete rather than serving the file. */
TEST(cache_busy_reader_gate_refuses_open) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_gatebusy"));
    char repo[700];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    char f[800];
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def a():\n    return 1\n"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    char *name = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(name);
    char db[900];
    snprintf(db, sizeof(db), "%s/%s.db", e.cache, name);

    cbm_db_lease_t *gate = NULL;
    ASSERT_EQ(cbm_db_read_gate_acquire(db, true, &gate), 0);
    cbm_store_t *s = cbm_store_open_path_reader(db);
    bool opened = s != NULL;
    cbm_store_close(s);
    s = cbm_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    bool held = cbm_store_hold_read_gate(s);
    cbm_store_close(s);
    cbm_db_lease_release(gate);
    ASSERT_FALSE(opened);
    ASSERT_FALSE(held);

    char to[900];
    snprintf(to, sizeof(to), "%s/ce-busy.db", e.cache);
    ASSERT_EQ(cbm_rename_replace(db, to), 0);
    ASSERT_EQ(cbm_db_read_gate_acquire(to, true, &gate), 0);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\"a\"}", name);
    char *resp = cbm_mcp_handle_tool(srv, "search_graph", args);
    cbm_db_lease_release(gate);
    ASSERT_NOT_NULL(resp);
    bool incomplete = strstr(resp, "lookup incomplete") != NULL;
    free(resp);
    resp = cbm_mcp_handle_tool(srv, "search_graph", args); /* the gate is free again */
    ASSERT_NOT_NULL(resp);
    bool served = strstr(resp, "not found") == NULL && strstr(resp, "incomplete") == NULL;
    free(resp);
    cbm_mcp_server_free(srv);
    ASSERT_TRUE(incomplete);
    ASSERT_TRUE(served);
    free(name);
    ce_teardown(&e);
    PASS();
}

/* The fallback scan reads a database, then takes its gate. When expiry moved
 * the file in between and a restore or re-index put another file at the
 * path, the gate must not adopt the connection: it reads the moved file. A
 * file that stayed (also one written in place) is still adopted. */
TEST(cache_reader_gate_refuses_replaced_file) {
#ifdef _WIN32
    PASS(); /* an open database cannot be moved there; no inode to compare */
#else
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_gaterepl"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "rp", root, db, sizeof(db));

    cbm_store_t *s = cbm_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    sqlite3 *w = NULL; /* an index run writing the same file in place */
    ASSERT_EQ(sqlite3_open(db, &w), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(w, "UPDATE projects SET indexed_at = '2026-02-02';", NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(w);
    bool kept = cbm_store_hold_read_gate(s);
    cbm_store_close(s);

    s = cbm_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    char moved[900];
    snprintf(moved, sizeof(moved), "%s/rp.db.moved", e.base);
    ASSERT_EQ(cbm_rename_replace(db, moved), 0);
    char again[800]; /* a re-index: built aside, renamed onto the path */
    ce_make_db_as(&e, "rp-new", "rp", root, again, sizeof(again));
    ASSERT_EQ(cbm_rename_replace(again, db), 0);
    bool adopted = cbm_store_hold_read_gate(s);
    cbm_store_close(s);

    ASSERT_TRUE(kept);
    ASSERT_FALSE(adopted);
    ce_teardown(&e);
    PASS();
#endif
}

#if defined(CBM_ENABLE_TEST_SEAMS) && !defined(_WIN32)
/* A copy of the database at src, taken with the backup API (WAL included). */
static bool ce_backup_db(const char *src, const char *dst) {
    sqlite3 *s = NULL;
    sqlite3 *d = NULL;
    bool ok = sqlite3_open(src, &s) == SQLITE_OK && sqlite3_open(dst, &d) == SQLITE_OK;
    sqlite3_backup *bk = ok ? sqlite3_backup_init(d, "main", s, "main") : NULL;
    ok = bk && sqlite3_backup_step(bk, -1) == SQLITE_DONE;
    if (bk) {
        ok = sqlite3_backup_finish(bk) == SQLITE_OK && ok;
    }
    sqlite3_close(d);
    sqlite3_close(s);
    return ok;
}

static bool ce_index_repo(const char *repo, char **project) {
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    if (!p) {
        return false;
    }
    bool ok = cbm_pipeline_run(p) == 0;
    if (project) {
        *project = strdup(cbm_pipeline_project_name(p));
    }
    cbm_pipeline_free(p);
    return ok;
}

typedef struct {
    const char *from;
    const char *to;
    int fired;
} ce_replace_t;

/* An index run replacing the database just after a tool call opened it. */
static void ce_replace_after_open(const char *db_path, void *ud) {
    ce_replace_t *r = (ce_replace_t *)ud;
    if (r->fired++ == 0 && strcmp(db_path, r->to) == 0) {
        (void)cbm_rename_replace(r->from, r->to);
    }
}
#endif

/* A tool call opens <project>.db, then (integrity check, project row) records
 * the generation of the file at the path. When an index run replaced the
 * file in between, the server must not take the new file's generation for
 * the connection's: every later call would keep reading the deleted file.
 * Found by scripts/branch-index-stress.py, where a server kept answering
 * with another branch's graph from a deleted inode. */
TEST(cache_store_generation_follows_opened_file) {
#if !defined(CBM_ENABLE_TEST_SEAMS) || defined(_WIN32)
    PASS(); /* needs the seam; no inode to compare on Windows */
#else
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_genrace"));
    char repo[700];
    char f[800];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    snprintf(f, sizeof(f), "%s/a.py", repo);
    ASSERT_EQ(th_write_file(f, "def alpha_old():\n    return 1\n"), 0);
    char *name = NULL;
    ASSERT_TRUE(ce_index_repo(repo, &name));
    char db[900];
    char v1[900];
    char v2[900];
    snprintf(db, sizeof(db), "%s/%s.db", e.cache, name);
    snprintf(v1, sizeof(v1), "%s/v1.copy", e.base);
    snprintf(v2, sizeof(v2), "%s/v2.copy", e.base);
    ASSERT_TRUE(ce_backup_db(db, v1));
    ASSERT_EQ(th_write_file(f, "def beta_new():\n    return 2\n"), 0);
    ASSERT_TRUE(ce_index_repo(repo, NULL));
    ASSERT_TRUE(ce_backup_db(db, v2));
    /* The live file holds the old graph; the replacement waits aside. */
    cbm_remove_db_sidecars(db);
    ASSERT_EQ(cbm_rename_replace(v1, db), 0);

    ce_replace_t r = {v2, db, 0};
    cbm_mcp_test_store_open_hook(ce_replace_after_open, &r);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\"beta_new\"}", name);
    char *first = cbm_mcp_handle_tool(srv, "search_graph", args); /* opens v1; v2 lands */
    char *second = cbm_mcp_handle_tool(srv, "search_graph", args);
    cbm_mcp_test_store_open_hook(NULL, NULL);
    cbm_mcp_server_free(srv);
    ASSERT_EQ(r.fired >= 1, true);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    bool served_new = strstr(second, "\"beta_new\"") != NULL;
    free(first);
    free(second);
    free(name);
    ce_teardown(&e);
    ASSERT_TRUE(served_new);
    PASS();
#endif
}

/* ── The server's background sweep ─────────────────────────────── */

/* Enable automatic sweeps (test_main turns them off for the whole run) and
 * the update check off; restore both afterwards. */
typedef struct {
    char expiry[64];
    char update[64];
    bool had_expiry;
    bool had_update;
} ce_srv_env_t;

static void ce_srv_env_enter(ce_srv_env_t *s) {
    s->had_expiry = cbm_safe_getenv("CBM_CACHE_EXPIRY", s->expiry, sizeof(s->expiry), NULL);
    s->had_update = cbm_safe_getenv("CBM_UPDATE_CHECK", s->update, sizeof(s->update), NULL);
    cbm_setenv("CBM_CACHE_EXPIRY", "1", 1);
    cbm_setenv("CBM_UPDATE_CHECK", "0", 1);
}

static void ce_srv_env_leave(const ce_srv_env_t *s) {
    if (s->had_expiry) {
        cbm_setenv("CBM_CACHE_EXPIRY", s->expiry, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_EXPIRY");
    }
    if (s->had_update) {
        cbm_setenv("CBM_UPDATE_CHECK", s->update, 1);
    } else {
        cbm_unsetenv("CBM_UPDATE_CHECK");
    }
}

static const char CE_INITIALIZE[] =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
    "\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";

/* A fresh server (calloc'd struct, constructed sweep flag) starts a sweep on
 * initialize, and join_threads waits for it to record last_sweep. */
TEST(cache_server_initialize_runs_sweep) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_srv"));
    ce_srv_env_t env;
    ce_srv_env_enter(&env);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    ASSERT_TRUE(cbm_cache_expiry_due(&cfg, (int64_t)time(NULL)));
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(srv, CE_INITIALIZE);
    ASSERT_NOT_NULL(resp);
    free(resp);
    cbm_mcp_server_join_threads(srv);
    ASSERT_FALSE(cbm_cache_expiry_due(&cfg, (int64_t)time(NULL)));
    cbm_mcp_server_join_threads(srv); /* idempotent */
    cbm_mcp_server_free(srv);
    ce_srv_env_leave(&env);
    ce_teardown(&e);
    PASS();
}

/* A server started below the repository root exempts the index the cwd
 * walk-up resolves to (the ancestor's), not the name derived from cwd: an
 * unused-for-long ancestor index must survive its own session's first sweep. */
TEST(cache_server_sweep_keeps_ancestor_index) {
#ifndef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_sub"));
    char repo[700];
    char sub[800];
    snprintf(repo, sizeof(repo), "%s/repo", e.base);
    snprintf(sub, sizeof(sub), "%s/src", repo);
    cbm_mkdir_p(sub, 0755);
    char *real = realpath(repo, NULL);
    ASSERT_NOT_NULL(real);
    char *name = cbm_project_name_from_path(real);
    ASSERT_NOT_NULL(name);
    char db[900];
    ce_make_db(&e, name, real, db, sizeof(db));
    int64_t now = (int64_t)time(NULL);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 200 * CE_DAY, NULL));
    char old_cwd[1024];
    ASSERT_NOT_NULL(getcwd(old_cwd, sizeof(old_cwd)));
    char real_sub[900];
    snprintf(real_sub, sizeof(real_sub), "%s/src", real);
    ASSERT_EQ(chdir(real_sub), 0);
    cbm_mcp_reset_cwd_project_cache();
    ce_srv_env_t env;
    ce_srv_env_enter(&env);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    ASSERT_TRUE(cbm_cache_expiry_due(&cfg, now));
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(srv, CE_INITIALIZE);
    ASSERT_NOT_NULL(resp);
    free(resp);
    cbm_mcp_server_join_threads(srv);
    cbm_mcp_server_free(srv);
    ce_srv_env_leave(&env);
    (void)chdir(old_cwd);
    cbm_mcp_reset_cwd_project_cache();
    ASSERT_FALSE(cbm_cache_expiry_due(&cfg, now)); /* the sweep ran */
    ASSERT_TRUE(cbm_file_exists(db));              /* and kept the session's index */
    ASSERT_EQ(ce_trash_count(&e), 0);
    free(name);
    free(real);
    ce_teardown(&e);
#endif
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
typedef struct {
    std::atomic<int> started;
    std::atomic<int> finished;
} ce_sweep_probe_t;

static void ce_sweep_probe(int stage, void *data) {
    ce_sweep_probe_t *p = (ce_sweep_probe_t *)data;
    if (stage == 0) {
        p->started.store(1);
        cbm_usleep(300000); /* hold the sweep while the test drops the watcher */
    } else {
        p->finished.store(1);
    }
}

static void ce_sweep_throws_once(int stage, void *data) {
    std::atomic<int> *calls = (std::atomic<int> *)data;
    if (stage == 0 && calls->fetch_add(1) == 0) {
        throw std::bad_alloc();
    }
}

/* A sweep that throws (bad_alloc in the scan) must not take the server down:
 * the thread logs it, clears its running flag and frees its job, so the next
 * initialize starts a sweep again. */
TEST(cache_server_sweep_survives_exception) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_srvx"));
    ce_srv_env_t env;
    ce_srv_env_enter(&env);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    std::atomic<int> calls{0};
    cbm_mcp_test_sweep_hook(ce_sweep_throws_once, &calls);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(srv, CE_INITIALIZE);
    free(resp);
    cbm_mcp_server_join_threads(srv);
    ASSERT_EQ(calls.load(), 1);
    ASSERT_TRUE(cbm_cache_expiry_due(&cfg, (int64_t)time(NULL))); /* that sweep recorded nothing */
    resp = cbm_mcp_server_handle(srv, CE_INITIALIZE);
    free(resp);
    cbm_mcp_server_join_threads(srv);
    cbm_mcp_test_sweep_hook(NULL, NULL);
    ASSERT_EQ(calls.load(), 2); /* a second sweep started */
    ASSERT_FALSE(cbm_cache_expiry_due(&cfg, (int64_t)time(NULL)));
    cbm_mcp_server_free(srv);
    ce_srv_env_leave(&env);
    ce_teardown(&e);
    PASS();
}

/* The sweep reads the externally owned watcher. Dropping the watcher must
 * wait for a running sweep, so the caller can free the watcher right after
 * (main's shutdown order); the sweep still saw the watcher and kept its
 * project. */
TEST(cache_server_sweep_joined_before_watcher_drop) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_srvw"));
    ce_srv_env_t env;
    ce_srv_env_enter(&env);
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "ce-watched", root, db, sizeof(db));
    ASSERT_TRUE(cbm_cache_usage_set(db, (int64_t)time(NULL) - 200 * CE_DAY, NULL));

    ce_sweep_probe_t probe;
    probe.started.store(0);
    probe.finished.store(0);
    cbm_mcp_test_sweep_hook(ce_sweep_probe, &probe);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_watcher_t *w = cbm_watcher_new(NULL, NULL, NULL);
    cbm_watcher_watch(w, "ce-watched", root);
    cbm_mcp_server_set_watcher(srv, w);
    char *resp = cbm_mcp_server_handle(srv, CE_INITIALIZE);
    free(resp);
    for (int i = 0; i < 200 && !probe.started.load(); i++) {
        cbm_usleep(10000);
    }
    ASSERT_EQ(probe.started.load(), 1);
    cbm_mcp_server_set_watcher(srv, NULL);
    ASSERT_EQ(probe.finished.load(), 1); /* joined, not still running */
    cbm_watcher_free(w);
    ASSERT_TRUE(cbm_file_exists(db)); /* the sweep consulted the watcher */
    cbm_mcp_server_free(srv);
    cbm_mcp_test_sweep_hook(NULL, NULL);
    ce_srv_env_leave(&env);
    ce_teardown(&e);
    PASS();
}
#endif

/* ── Fixes that landed without a test of their own ──────────────── */

static bool ce_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out = in ? fopen(dst, "wb") : NULL;
    bool ok = in && out;
    char buf[8192];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    ok = ok && !ferror(in);
    if (out && fclose(out) != 0) {
        ok = false;
    }
    if (in) {
        fclose(in);
    }
    return ok;
}

/* A database whose last commit is only in its WAL, copied to dst (the main
 * file) and dst-wal while the connection still holds both: closing it would
 * checkpoint the WAL away. Table t holds 'in-main' and 'in-wal'. */
static bool ce_make_db_with_wal(const char *scratch, const char *dst) {
    sqlite3 *db = NULL;
    bool ok = sqlite3_open(scratch, &db) == SQLITE_OK &&
              sqlite3_exec(db,
                           "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
                           "CREATE TABLE t(v TEXT); INSERT INTO t VALUES('in-main');"
                           "PRAGMA wal_checkpoint(TRUNCATE);"
                           "INSERT INTO t VALUES('in-wal');",
                           NULL, NULL, NULL) == SQLITE_OK;
    std::string wal = std::string(scratch) + "-wal";
    ok = ok && cbm_file_size(wal.c_str()) > 0 && ce_copy_file(scratch, dst) &&
         ce_copy_file(wal.c_str(), (std::string(dst) + "-wal").c_str());
    sqlite3_close(db);
    return ok;
}

/* Rows of table t in the database at path (v = value; every row when value
 * is NULL), -1 when unreadable. */
static int ce_rows(const char *path, const char *value) {
    sqlite3 *db = NULL;
    int n = -1;
    if (sqlite3_open(path, &db) == SQLITE_OK) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM t WHERE ?1 IS NULL OR v = ?1;", -1, &st,
                               NULL) == SQLITE_OK) {
            if (value) {
                sqlite3_bind_text(st, 1, value, -1, SQLITE_STATIC);
            }
            if (sqlite3_step(st) == SQLITE_ROW) {
                n = sqlite3_column_int(st, 0);
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return n;
}

/* cache restore puts the saved WAL back before the main file: when the WAL
 * cannot go back (here a directory stands where it goes), the main file
 * stays in the trash with it rather than being restored without the commits
 * only the WAL holds. Once the way is clear, the restored database has them. */
TEST(cache_trash_restore_puts_wal_back_first) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rswal"));
    int64_t now = (int64_t)time(NULL);
    char entry[800];
    snprintf(entry, sizeof(entry), "%s/trash/%lld-rw", e.cache, (long long)(now - CE_DAY));
    ASSERT_TRUE(cbm_mkdir_p(entry, 0755));
    char scratch[700];
    snprintf(scratch, sizeof(scratch), "%s/scratch.db", e.base);
    char saved[900];
    char saved_wal[900];
    snprintf(saved, sizeof(saved), "%s/rw.db", entry);
    snprintf(saved_wal, sizeof(saved_wal), "%s/rw.db-wal", entry);
    ASSERT_TRUE(ce_make_db_with_wal(scratch, saved));
    char live[800];
    char live_wal[800];
    char block[900];
    snprintf(live, sizeof(live), "%s/rw.db", e.cache);
    snprintf(live_wal, sizeof(live_wal), "%s/rw.db-wal", e.cache);
    snprintf(block, sizeof(block), "%s/x", live_wal);
    ASSERT_TRUE(cbm_mkdir_p(block, 0755));
    char msg[512];
    ASSERT_NEQ(cbm_cache_trash_restore("rw", msg, sizeof(msg)), 0);
    ASSERT_FALSE(cbm_file_exists(live));
    ASSERT_TRUE(cbm_file_exists(saved));
    ASSERT_TRUE(cbm_file_exists(saved_wal));
    th_rmtree(live_wal);
    ASSERT_EQ(cbm_cache_trash_restore("rw", msg, sizeof(msg)), 0);
    ASSERT_EQ(ce_rows(live, "in-wal"), 1);
    ASSERT_EQ(ce_rows(live, NULL), 2);
    ce_teardown(&e);
    PASS();
}

/* A restore whose main file does not go back (here it is missing from the
 * entry) moves the WAL it already put back into the entry again: no WAL is
 * left beside a database that is not there. */
TEST(cache_trash_restore_failed_main_moves_wal_back) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rsundo"));
    int64_t now = (int64_t)time(NULL);
    char entry[800];
    snprintf(entry, sizeof(entry), "%s/trash/%lld-ru", e.cache, (long long)(now - CE_DAY));
    ASSERT_TRUE(cbm_mkdir_p(entry, 0755));
    char scratch[700];
    snprintf(scratch, sizeof(scratch), "%s/scratch.db", e.base);
    char saved[900];
    char saved_wal[900];
    snprintf(saved, sizeof(saved), "%s/ru.db", entry);
    snprintf(saved_wal, sizeof(saved_wal), "%s/ru.db-wal", entry);
    ASSERT_TRUE(ce_make_db_with_wal(scratch, saved));
    ASSERT_EQ(cbm_unlink(saved), 0);
    char live[800];
    char live_wal[800];
    snprintf(live, sizeof(live), "%s/ru.db", e.cache);
    snprintf(live_wal, sizeof(live_wal), "%s/ru.db-wal", e.cache);
    char msg[512];
    ASSERT_NEQ(cbm_cache_trash_restore("ru", msg, sizeof(msg)), 0);
    ASSERT_FALSE(cbm_file_exists(live));
    ASSERT_FALSE(cbm_file_exists(live_wal));
    ASSERT_TRUE(cbm_file_exists(saved_wal));
    ce_teardown(&e);
    PASS();
}

/* cbm_config_get's value is the calling thread's: two threads reading
 * different keys through one handle (the server's auto-index and sweep
 * threads beside the main thread) never see each other's value. */
TEST(cache_config_get_value_is_per_thread) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_cfgthr"));
    cbm_config_t *cfg = cbm_config_open(e.cache);
    ASSERT_NOT_NULL(cfg);
    std::string va(300, 'a');
    std::string vb(37, 'b');
    ASSERT_EQ(cbm_config_set(cfg, "ce_thread_a", va.c_str()), 0);
    ASSERT_EQ(cbm_config_set(cfg, "ce_thread_b", vb.c_str()), 0);
    std::atomic<bool> go{false};
    std::atomic<int> wrong{0};
    auto reader = [&](const char *key, const std::string &want) {
        while (!go.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 20000; i++) {
            const char *v = cbm_config_get(cfg, key, "");
            if (want != v) {
                wrong.fetch_add(1);
            }
        }
    };
    std::thread ta([&] { reader("ce_thread_a", va); });
    std::thread tb([&] { reader("ce_thread_b", vb); });
    go.store(true);
    ta.join();
    tb.join();
    cbm_config_close(cfg);
    ASSERT_EQ(wrong.load(), 0);
    ce_teardown(&e);
    PASS();
}

/* root_path and missing_checks of the usage row keyed `key` ("" and -1 when
 * absent). */
static void ce_usage_root(const ce_env_t *e, const char *key, std::string *root, int *checks) {
    *root = "";
    *checks = -1;
    char p[700];
    snprintf(p, sizeof(p), "%s/_config.db", e->cache);
    sqlite3 *db = NULL;
    sqlite3_open(p, &db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT root_path, missing_checks FROM cache_usage WHERE db_name=?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *r = (const char *)sqlite3_column_text(st, 0);
            *root = r ? r : "";
            *checks = sqlite3_column_int(st, 1);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/* A query touch (no root) racing an index run's usage write never puts the
 * old root and missing streak back: both write under the write lock, the
 * touch re-reading the row there. Each round starts both at once. */
TEST(cache_usage_write_keeps_concurrent_root) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_rmw"));
    enum { ROUNDS = 200 };
    int64_t now = (int64_t)time(NULL);
    std::vector<std::string> dbs;
    for (int k = 0; k < ROUNDS; k++) {
        dbs.push_back(std::string(e.cache) + "/rmwrace" + std::to_string(k) + ".db");
        /* Last used two hours ago, so the touch writes. */
        ASSERT_TRUE(cbm_cache_usage_set(dbs[k].c_str(), now - 7200, "/old/root"));
    }
    char cp[700];
    snprintf(cp, sizeof(cp), "%s/_config.db", e.cache);
    sqlite3 *h = NULL;
    ASSERT_EQ(sqlite3_open(cp, &h), SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE cache_usage SET missing_since = %lld, missing_checks = 2 "
             "WHERE db_name LIKE 'rmwrace%%';",
             (long long)(now - CE_DAY));
    ASSERT_EQ(sqlite3_exec(h, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(h);

    std::atomic<int> arrived{0};
    auto start_round = [&](int k) {
        arrived.fetch_add(1);
        while (arrived.load() < 2 * (k + 1)) {
            std::this_thread::yield();
        }
    };
    std::thread toucher([&] {
        for (int k = 0; k < ROUNDS; k++) {
            start_round(k);
            cbm_cache_usage_touch(dbs[k].c_str(), NULL);
        }
    });
    std::thread indexer([&] {
        for (int k = 0; k < ROUNDS; k++) {
            start_round(k);
            (void)cbm_cache_usage_set(dbs[k].c_str(), 0, "/new/root");
        }
    });
    toucher.join();
    indexer.join();
    int stale = 0;
    for (int k = 0; k < ROUNDS; k++) {
        std::string key = "rmwrace" + std::to_string(k) + ".db";
        std::string root;
        int checks;
        ce_usage_root(&e, key.c_str(), &root, &checks);
        stale += root != "/new/root" || checks != 0;
    }
    ASSERT_EQ(stale, 0);
    ce_teardown(&e);
    PASS();
}

/* An orphan sidecar whose unlink is refused (a read-only cache directory)
 * is neither reported nor counted; once the directory is writable again it
 * is. */
TEST(cache_orphan_sidecar_unlink_refused_not_reported) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root ignores directory permissions */
    }
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_sidero"));
    int64_t now = (int64_t)time(NULL);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0); /* the sweep's own lock file */
    char side[800];
    char gone[800];
    snprintf(side, sizeof(side), "%s/gone.db-wal", e.cache);
    snprintf(gone, sizeof(gone), "%s/gone.db", e.cache);
    ASSERT_EQ(th_write_file(side, "x"), 0);
    ce_age(side, now - 2 * 3600);
    /* The lease's lock file is there already (it is never unlinked), so
     * the lease needs no new entry in the read-only directory. */
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(gone, &lease), 0);
    cbm_db_lease_release(lease);
    ASSERT_EQ(chmod(e.cache, 0555), 0);
    int rc = ce_sweep(&cfg, now, false, &r);
    chmod(e.cache, 0755);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.sidecars, 0);
    ASSERT_EQ(r.bytes, 0);
    ASSERT_TRUE(cbm_file_exists(side));
    ASSERT_EQ(ce_sweep(&cfg, now, false, &r), 0);
    ASSERT_EQ(r.sidecars, 1);
    ASSERT_FALSE(cbm_file_exists(side));
    ce_teardown(&e);
#endif
    PASS();
}

/* Windows: a root on a UNC share (\\server\share\repo) is absolute: once
 * the checkout is gone below a share that is still there, its missing streak
 * starts. Uses the administrative share of the temp directory's drive. */
TEST(cache_root_gone_on_unc_share_counts) {
#ifdef _WIN32
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_unc"));
    if (!isalpha((unsigned char)e.base[0]) || e.base[1] != ':') {
        ce_teardown(&e);
        SKIP_PLATFORM("temp directory is not on a drive letter");
    }
    char unc_base[700];
    snprintf(unc_base, sizeof(unc_base), "\\\\localhost\\%c$%s", e.base[0], e.base + 2);
    for (char *c = unc_base; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    if (!cbm_is_dir(unc_base)) {
        ce_teardown(&e);
        SKIP_PLATFORM("no administrative share \\\\localhost\\<drive>$");
    }
    char root[800];
    snprintf(root, sizeof(root), "%s\\gone\\repo", unc_base);
    char db[800];
    ce_make_db(&e, "unc", root, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "unc", &lu, &checks);
    ASSERT_EQ(checks, 1); /* missing, not uncertain */
    ce_teardown(&e);
#endif
    PASS();
}

/* A WAL that can neither be moved into the trash nor deleted (here a
 * directory stands in its place, and another at its destination) is never
 * left beside a missing database: the main file goes back and the removal
 * fails. Once the WAL can go, the removal goes through. */
TEST(cache_remove_project_wal_stuck_rolls_back) {
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_walstuck"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    ce_make_db(&e, "ws", root, db, sizeof(db));
    char wal[900];
    char wal_in[1000];
    snprintf(wal, sizeof(wal), "%s-wal", db);
    snprintf(wal_in, sizeof(wal_in), "%s/x", wal);
    ASSERT_TRUE(cbm_mkdir_p(wal_in, 0755));
    int64_t now = (int64_t)time(NULL);
    char entry[800];
    snprintf(entry, sizeof(entry), "%s/trash/%lld-ws", e.cache, (long long)now);
    char blocked[1000];
    snprintf(blocked, sizeof(blocked), "%s/ws.db-wal/x", entry);
    ASSERT_TRUE(cbm_mkdir_p(blocked, 0755));
    char moved[900];
    snprintf(moved, sizeof(moved), "%s/ws.db", entry);
    int parked = -1;
    ASSERT_EQ(cbm_cache_remove_project(db, "ws", entry, &parked), -1);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_FALSE(cbm_file_exists(moved));
    th_rmtree(wal);
    ASSERT_EQ(cbm_cache_remove_project(db, "ws", entry, &parked), 0);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(moved));
    ce_teardown(&e);
    PASS();
}

#ifndef _WIN32
typedef struct {
    int n;
    bool removed;
    bool skipped;
    int64_t bytes;
    std::string reason;
} ce_item_rec_t;

static void ce_record_sl(const cbm_cache_expiry_item_t *it, void *ud) {
    ce_item_rec_t *rec = (ce_item_rec_t *)ud;
    if (it->kind == CBM_EXPIRE_UNUSED && it->name && strcmp(it->name, "sl") == 0) {
        rec->n++;
        rec->removed = it->removed;
        rec->skipped = it->skipped != NULL;
        rec->bytes = it->bytes;
        rec->reason = it->reason ? it->reason : "";
    }
}
#endif

/* A project whose database went but whose parked slot stayed (a read-only
 * slot directory) is reported removed -- not kept -- with the slots'
 * refusal in its reason, and only the bytes that went are counted. */
TEST(cache_sweep_reports_project_with_slots_left) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root ignores directory permissions */
    }
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_slleft"));
    char root[700];
    char db[800];
    snprintf(root, sizeof(root), "%s/repo", e.base);
    cbm_mkdir_p(root, 0755);
    int64_t now = (int64_t)time(NULL);
    ce_make_db(&e, "sl", root, db, sizeof(db));
    int64_t db_bytes = cbm_file_size(db);
    ASSERT_GT(db_bytes, 0);
    char sdir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(db, "sl", root, sdir, sizeof(sdir)));
    ASSERT_TRUE(cbm_mkdir_p(sdir, 0755));
    char slot[1300];
    snprintf(slot, sizeof(slot), "%s/main.db.zst", sdir);
    std::string big(256 * 1024, 'z');
    ASSERT_EQ(th_write_file(slot, big.c_str()), 0);
    ASSERT_TRUE(cbm_cache_usage_set(db, now - 100 * CE_DAY, NULL));
    /* The project's slot directory cannot move into the trash (a directory
     * changing parent needs write access to itself), and the slot in its
     * root's directory cannot be deleted. */
    std::string pdir(sdir, strrchr(sdir, '/') - sdir);
    ASSERT_EQ(chmod(sdir, 0555), 0);
    ASSERT_EQ(chmod(pdir.c_str(), 0555), 0);
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ce_item_rec_t rec = {0, false, false, 0, ""};
    cbm_cache_sweep_opts_t o = {};
    o.now = now;
    o.report = ce_record_sl;
    o.report_ud = &rec;
    int rc = cbm_cache_expiry_sweep(&cfg, &o, &r);
    chmod(pdir.c_str(), 0755);
    chmod(sdir, 0755);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(rec.n, 1);
    ASSERT_TRUE(rec.removed);
    ASSERT_FALSE(rec.skipped);
    ASSERT_TRUE(rec.reason.find("could not be removed") != std::string::npos);
    ASSERT_EQ(rec.bytes, db_bytes); /* the database only: the slot stayed */
    ASSERT_EQ(r.projects, 1);
    ASSERT_EQ(r.skipped, 0);
    ASSERT_EQ(r.bytes, db_bytes);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_TRUE(cbm_file_exists(slot));
    ce_teardown(&e);
#endif
    PASS();
}

#ifdef __linux__
/* A mount point named in /proc/self/mounts that is none of the fixed mount
 * parents, has entries (so the empty-directory rule cannot decide) and has
 * no entry `child`. "" when there is none. */
static std::string ce_listed_mount_point(const char *child) {
    static const char *const parents[] = {"/",
                                          "/Volumes",
                                          "/mnt",
                                          "/media",
                                          "/run/media",
                                          "/net",
                                          "/Network",
                                          "/System/Volumes",
                                          "/private/var/automount",
                                          "/afs",
                                          "/cifs",
                                          "/nfs"};
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f) {
        return "";
    }
    std::string found;
    char line[4096];
    while (found.empty() && fgets(line, sizeof(line), f)) {
        char dev[1024];
        char mnt[2048];
        if (sscanf(line, "%1023s %2047s", dev, mnt) != 2 || strchr(mnt, '\\')) {
            continue; /* an escaped name (a space in it) */
        }
        bool parent = strncmp(mnt, "/media/", 7) == 0 || strncmp(mnt, "/run/media/", 11) == 0;
        for (const char *p : parents) {
            parent = parent || strcmp(mnt, p) == 0;
        }
        if (parent || !cbm_is_dir(mnt)) {
            continue;
        }
        bool any = false;
        cbm_dir_t *d = cbm_opendir(mnt);
        if (d) {
            cbm_dirent_t *ent;
            while (!any && (ent = cbm_readdir(d)) != NULL) {
                any = strcmp(ent->name, ".") != 0 && strcmp(ent->name, "..") != 0;
            }
            cbm_closedir(d);
        }
        std::string probe = std::string(mnt) + "/" + child;
        struct stat st;
        if (any && stat(probe.c_str(), &st) != 0 && errno == ENOENT) {
            found = mnt;
        }
    }
    fclose(f);
    return found;
}
#endif

/* A missing root whose nearest existing ancestor is a mount point in the
 * mount table (not one of the fixed mount parents, and not empty) is
 * unavailable, not deleted: no missing streak. A root below an ordinary
 * directory still starts one. */
TEST(cache_root_under_listed_mount_point_is_uncertain) {
#ifdef __linux__
    char child[64];
    snprintf(child, sizeof(child), "cbm-no-such-%d", (int)getpid());
    std::string mnt = ce_listed_mount_point(child);
    if (mnt.empty()) {
        FAIL("no usable mount point in /proc/self/mounts");
    }
    ce_env_t e;
    ASSERT_TRUE(ce_setup(&e, "cbm_ce_mtab"));
    char r1[2200];
    snprintf(r1, sizeof(r1), "%s/%s/repo", mnt.c_str(), child);
    char busy[700];
    snprintf(busy, sizeof(busy), "%s/work/other-repo", e.base);
    ASSERT_TRUE(cbm_mkdir_p(busy, 0755));
    char r2[800];
    snprintf(r2, sizeof(r2), "%s/work/repo", e.base);
    char db[800];
    ce_make_db(&e, "unmounted", r1, db, sizeof(db));
    ce_make_db(&e, "deleted", r2, db, sizeof(db));
    cbm_cache_expiry_cfg_t cfg;
    ce_cfg(&cfg);
    cbm_cache_sweep_result_t r;
    ASSERT_EQ(ce_sweep(&cfg, (int64_t)time(NULL), false, &r), 0);
    int64_t lu;
    int checks;
    ce_usage(&e, "unmounted", &lu, &checks);
    ASSERT_EQ(checks, 0);
    ce_usage(&e, "deleted", &lu, &checks);
    ASSERT_EQ(checks, 1);
    ce_teardown(&e);
#endif
    PASS();
}

SUITE(cache_expiry) {
    RUN_TEST(cache_root_gone_needs_streak_and_grace);
    RUN_TEST(cache_root_gone_but_used_is_kept);
    RUN_TEST(cache_root_back_before_move_is_kept);
    RUN_TEST(cache_cli_now_needs_dry_run);
    RUN_TEST(cache_uninstall_removes_trash_and_rows);
    RUN_TEST(cache_root_back_resets_streak);
    RUN_TEST(cache_root_uncertain_never_counts);
    RUN_TEST(cache_truncated_memo_root_is_not_trusted);
    RUN_TEST(cache_root_under_empty_ancestor_is_uncertain);
    RUN_TEST(cache_removed_worktree_under_empty_dir_is_missing);
    RUN_TEST(cache_root_behind_dangling_link_is_uncertain);
    RUN_TEST(cache_root_unicode_and_long_is_present);
    RUN_TEST(cache_mass_missing_and_env_change_count_nothing);
    RUN_TEST(cache_refuses_unknown_cache_dir);
    RUN_TEST(cache_unused_expires);
    RUN_TEST(cache_foreign_db_files_are_not_judged);
    RUN_TEST(cache_keeps_session_and_watched);
    RUN_TEST(cache_root_gone_windows_separators);
    RUN_TEST(cache_slot_unlink_refused_not_counted);
    RUN_TEST(cache_skips_leased_project);
    RUN_TEST(cache_skips_database_with_open_reader);
    RUN_TEST(cache_dry_run_changes_nothing);
    RUN_TEST(cache_dry_run_reports_root_gone_due);
    RUN_TEST(cache_dry_run_leaves_memo_schema);
    RUN_TEST(cache_dry_run_counts_project_slots_once);
    RUN_TEST(cache_dry_run_applies_safety_checks_and_creates_nothing);
    RUN_TEST(cache_trash_restore_and_purge);
    RUN_TEST(cache_trash_purge_skips_leased_project);
    RUN_TEST(cache_renamed_db_kept_by_project);
    RUN_TEST(cache_renamed_db_expires_as_project);
#ifndef _WIN32
    RUN_TEST(cache_remove_project_reports_slots_left);
    RUN_TEST(cache_trash_purge_never_follows_links);
    RUN_TEST(cache_trash_purge_refused_is_skipped);
    RUN_TEST(cache_trash_root_link_is_refused);
    RUN_TEST(cache_slots_dir_link_is_refused);
    RUN_TEST(cache_linked_db_and_slot_are_ignored);
    RUN_TEST(cache_old_slots_expire);
    RUN_TEST(cache_orphan_slot_sidecars_expire);
    RUN_TEST(cache_orphan_sidecars);
#endif
    RUN_TEST(cache_usage_touch_is_throttled);
    RUN_TEST(cache_usage_touch_retries_after_busy);
    RUN_TEST(cache_usage_concurrent_writers_all_recorded);
    RUN_TEST(cache_used_since_scan_is_kept);
    RUN_TEST(cache_touch_during_scan_lands);
    RUN_TEST(cache_index_and_query_record_use);
    RUN_TEST(cache_autoindex_in_process_records_use);
    RUN_TEST(cache_pipeline_on_indexed_holds_lease);
    RUN_TEST(cache_pipeline_on_indexed_after_failed_export);
    RUN_TEST(cache_index_use_recorded_before_lease_release);
    RUN_TEST(cache_query_on_renamed_db_records_use);
    RUN_TEST(cache_open_reader_keeps_database);
    RUN_TEST(cache_open_reader_keeps_renamed_database);
    RUN_TEST(cache_renamed_db_waits_for_project_lease);
    RUN_TEST(cache_renamed_db_with_unsafe_stem_expires);
    RUN_TEST(cache_busy_reader_gate_refuses_open);
    RUN_TEST(cache_reader_gate_refuses_replaced_file);
    RUN_TEST(cache_store_generation_follows_opened_file);
    RUN_TEST(cache_server_initialize_runs_sweep);
    RUN_TEST(cache_server_sweep_keeps_ancestor_index);
    RUN_TEST(cache_trash_restore_puts_wal_back_first);
    RUN_TEST(cache_trash_restore_failed_main_moves_wal_back);
    RUN_TEST(cache_config_get_value_is_per_thread);
    RUN_TEST(cache_usage_write_keeps_concurrent_root);
    RUN_TEST(cache_orphan_sidecar_unlink_refused_not_reported);
    RUN_TEST(cache_root_gone_on_unc_share_counts);
    RUN_TEST(cache_remove_project_wal_stuck_rolls_back);
    RUN_TEST(cache_sweep_reports_project_with_slots_left);
    RUN_TEST(cache_root_under_listed_mount_point_is_uncertain);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(cache_server_sweep_joined_before_watcher_drop);
    RUN_TEST(cache_server_sweep_survives_exception);
#endif
}
