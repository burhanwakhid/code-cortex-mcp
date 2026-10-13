/*
 * test_artifact.c — Tests for persistent artifact export/import.
 */
#include "test_framework.h"
#include "store/store.h"
#include "pipeline/artifact.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h" /* cbm_file_exists */

#include <sys/stat.h>
#include <stdio.h>
#ifdef _WIN32
#include <process.h> /* _getpid */
#define getpid _getpid
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <thread>
#include <vector>

/* ── Helpers ─────────────────────────────────────────────────────── */

static char g_tmpdir[1024];
static char g_repo[1024];
static char g_db[1024];

static void setup_artifact_test(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/cbm_test_artifact_XXXXXX", cbm_tmpdir());
    cbm_mkdtemp(g_tmpdir);

    snprintf(g_repo, sizeof(g_repo), "%s/repo", g_tmpdir);
    cbm_mkdir_p(g_repo, 0755);

    snprintf(g_db, sizeof(g_db), "%s/test.db", g_tmpdir);
}

/* Create a minimal but valid DB with some nodes and edges. */
static void create_test_db(const char *path) {
    cbm_store_t *s = cbm_store_open_path(path);
    if (!s) {
        return;
    }

    cbm_store_exec(s, "INSERT OR IGNORE INTO projects(name, indexed_at, root_path) "
                      "VALUES('test-proj', '2026-01-01', '/tmp/test');");

    cbm_store_exec(s, "INSERT INTO nodes(project, label, name, qualified_name, file_path) "
                      "VALUES('test-proj', 'Function', 'foo', 'test-proj.foo', 'main.c');");
    cbm_store_exec(s, "INSERT INTO nodes(project, label, name, qualified_name, file_path) "
                      "VALUES('test-proj', 'Function', 'bar', 'test-proj.bar', 'main.c');");

    cbm_store_exec(s, "INSERT INTO edges(project, source_id, target_id, type) "
                      "VALUES('test-proj', 1, 2, 'CALLS');");

    cbm_store_close(s);
}

static void cleanup_dir(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/* ── Tests ───────────────────────────────────────────────────────── */

/* Two exports at once (two projects in one server, or a server and the
 * CLI) must each snapshot into their own temp file: with one shared name,
 * the second VACUUM INTO fails or reads the first one's bytes, and the
 * first one's unlink pulls the file out from under the second. */
TEST(artifact_concurrent_exports_do_not_collide) {
    setup_artifact_test();
    enum { N = 4 };
    char repos[N][1024];
    char dbs[N][1024];
    for (int i = 0; i < N; i++) {
        snprintf(repos[i], sizeof(repos[i]), "%s/repo%d", g_tmpdir, i);
        snprintf(dbs[i], sizeof(dbs[i]), "%s/test%d.db", g_tmpdir, i);
        cbm_mkdir_p(repos[i], 0755);
        create_test_db(dbs[i]);
    }

    int rcs[N];
    std::vector<std::thread> threads;
    for (int i = 0; i < N; i++) {
        threads.emplace_back([&, i] {
            /* BEST strips indexes and VACUUMs the copy, the longer race window. */
            rcs[i] = cbm_artifact_export(dbs[i], repos[i], "test-proj", CBM_ARTIFACT_BEST);
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(rcs[i], 0);
        char import_db[1024];
        snprintf(import_db, sizeof(import_db), "%s/imported%d.db", g_tmpdir, i);
        ASSERT_EQ(cbm_artifact_import(repos[i], import_db), 0);
        cbm_store_t *s = cbm_store_open_path(import_db);
        ASSERT_NOT_NULL(s);
        ASSERT_EQ(cbm_store_count_nodes(s, "test-proj"), 2);
        ASSERT_EQ(cbm_store_count_edges(s, "test-proj"), 1);
        cbm_store_close(s);
    }
    cleanup_dir(g_tmpdir);
    PASS();
}

/* Exports of different graphs into ONE repository at once (two projects
 * indexed from one path under different names) publish one consistent pair:
 * the metadata's node count is the payload's. Without the per-repository
 * publish lock they shared the temp names and could pair one snapshot's
 * payload with another's metadata. */
TEST(artifact_concurrent_exports_one_repo_publish_one_pair) {
    setup_artifact_test();
    enum { N = 4 };
    char repo[1024];
    snprintf(repo, sizeof(repo), "%s/shared-repo", g_tmpdir);
    cbm_mkdir_p(repo, 0755);
    char dbs[N][1024];
    for (int i = 0; i < N; i++) {
        snprintf(dbs[i], sizeof(dbs[i]), "%s/shared%d.db", g_tmpdir, i);
        create_test_db(dbs[i]);
        cbm_store_t *s = cbm_store_open_path(dbs[i]);
        for (int k = 0; k < i * 3; k++) { /* graph i has 2 + 3i nodes */
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO nodes(project, label, name, qualified_name, file_path) "
                     "VALUES('test-proj', 'Function', 'x%d', 'test-proj.x%d', 'x.c');",
                     k, k);
            cbm_store_exec(s, sql);
        }
        cbm_store_close(s);
    }
    int rcs[N];
    std::vector<std::thread> threads;
    for (int i = 0; i < N; i++) {
        threads.emplace_back(
            [&, i] { rcs[i] = cbm_artifact_export(dbs[i], repo, "test-proj", CBM_ARTIFACT_BEST); });
    }
    for (auto &t : threads) {
        t.join();
    }
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(rcs[i], 0);
    }
    char meta[1100];
    snprintf(meta, sizeof(meta), "%s/.code-cortex/artifact.json", repo);
    FILE *f = fopen(meta, "rb");
    ASSERT_NOT_NULL(f);
    char text[8192] = {0};
    size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    ASSERT_TRUE(got > 0);
    const char *nk = strstr(text, "\"nodes\":");
    ASSERT_NOT_NULL(nk);
    int meta_nodes = atoi(nk + strlen("\"nodes\":"));
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/shared-imported.db", g_tmpdir);
    ASSERT_EQ(cbm_artifact_import(repo, import_db), 0);
    cbm_store_t *s = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(s);
    int payload_nodes = cbm_store_count_nodes(s, "test-proj");
    cbm_store_close(s);
    ASSERT_EQ(meta_nodes, payload_nodes);
    cleanup_dir(g_tmpdir);
    PASS();
}

/* No publish lock to be had (a read-only cache): the export fails closed
 * rather than publishing unlocked through the shared temp names. */
TEST(artifact_export_fails_closed_without_publish_lock) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root ignores directory permissions */
    }
    setup_artifact_test();
    char repo[1024];
    char db[1024];
    char rocache[1024];
    snprintf(repo, sizeof(repo), "%s/lockless-repo", g_tmpdir);
    snprintf(db, sizeof(db), "%s/lockless.db", g_tmpdir);
    snprintf(rocache, sizeof(rocache), "%s/ro-cache", g_tmpdir);
    cbm_mkdir_p(repo, 0755);
    cbm_mkdir_p(rocache, 0755);
    create_test_db(db);
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", rocache, 1);
    ASSERT_EQ(chmod(rocache, 0555), 0);
    int rc = cbm_artifact_export(db, repo, "test-proj", CBM_ARTIFACT_FAST);
    chmod(rocache, 0755);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    ASSERT_NEQ(rc, 0);
    char meta[1100];
    snprintf(meta, sizeof(meta), "%s/.code-cortex/artifact.json", repo);
    ASSERT_FALSE(cbm_file_exists(meta));
    cleanup_dir(g_tmpdir);
#endif
    PASS();
}

/* An export removes the snapshot files (cbm_artifact_<pid>_<n>.db and its
 * sidecars) that a process which died mid-export left in the temp
 * directory; this process's own and a live process's stay. */
static long artifact_dead_pid(void) {
#ifdef _WIN32
    return 0x7FFFFFF0L; /* no such process: OpenProcess fails */
#else
    pid_t child = fork();
    if (child == 0) {
        _exit(0);
    }
    if (child < 0) {
        return -1;
    }
    int status = 0;
    (void)waitpid(child, &status, 0);
    return (long)child;
#endif
}

TEST(artifact_export_removes_orphaned_snapshots) {
    setup_artifact_test();
    create_test_db(g_db);
    long dead = artifact_dead_pid();
    ASSERT_GT(dead, 0);
    char orphan[1024];
    char orphan_wal[1100];
    char mine[1024];
    char unrelated[1024];
    snprintf(orphan, sizeof(orphan), "%s/cbm_artifact_%ld_7.db", cbm_tmpdir(), dead);
    snprintf(orphan_wal, sizeof(orphan_wal), "%s-wal", orphan);
    snprintf(mine, sizeof(mine), "%s/cbm_artifact_%ld_999999.db", cbm_tmpdir(), (long)getpid());
    snprintf(unrelated, sizeof(unrelated), "%s/cbm_artifact_%ld_7.dbx", cbm_tmpdir(), dead);
    const char *files[] = {orphan, orphan_wal, mine, unrelated};
    for (const char *f : files) {
        FILE *fp = fopen(f, "wb");
        ASSERT_NOT_NULL(fp);
        fputs("snapshot", fp);
        fclose(fp);
    }
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    ASSERT_FALSE(cbm_file_exists(orphan));
    ASSERT_FALSE(cbm_file_exists(orphan_wal));
    ASSERT_TRUE(cbm_file_exists(mine));      /* this process: maybe mid-export */
    ASSERT_TRUE(cbm_file_exists(unrelated)); /* not a snapshot name */
    (void)cbm_unlink(mine);
    (void)cbm_unlink(unrelated);
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_export_fast_roundtrip) {
    setup_artifact_test();
    create_test_db(g_db);

    /* Export with fast quality (zstd -3, no index stripping) */
    int rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    ASSERT_EQ(rc, 0);

    /* Verify artifact files exist */
    char zst[1024];
    snprintf(zst, sizeof(zst), "%s/.code-cortex/graph.db.zst", g_repo);
    struct stat st;
    ASSERT_EQ(stat(zst, &st), 0);
    ASSERT_GT((int)st.st_size, 0);

    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/.code-cortex/artifact.json", g_repo);
    ASSERT_EQ(stat(meta, &st), 0);

    /* Import to a new path */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_EQ(rc, 0);

    /* Verify imported DB has correct data */
    cbm_store_t *s = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(s);
    int nodes = cbm_store_count_nodes(s, "test-proj");
    int edges = cbm_store_count_edges(s, "test-proj");
    ASSERT_EQ(nodes, 2);
    ASSERT_EQ(edges, 1);
    cbm_store_close(s);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_export_best_roundtrip) {
    setup_artifact_test();
    create_test_db(g_db);

    /* Export with best quality (zstd -9, index stripping + VACUUM) */
    int rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_BEST);
    ASSERT_EQ(rc, 0);

    /* Source DB should be untouched (VACUUM INTO doesn't modify source) */
    cbm_store_t *src = cbm_store_open_path(g_db);
    ASSERT_NOT_NULL(src);
    ASSERT_EQ(cbm_store_count_nodes(src, "test-proj"), 2);
    cbm_store_close(src);

    /* Import and verify */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_count_nodes(s, "test-proj"), 2);
    ASSERT_EQ(cbm_store_count_edges(s, "test-proj"), 1);
    cbm_store_close(s);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_exists_check) {
    setup_artifact_test();
    create_test_db(g_db);

    /* No artifact yet */
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    /* Export creates the artifact */
    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    ASSERT_TRUE(cbm_artifact_exists(g_repo));

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_commit_hash) {
    setup_artifact_test();
    create_test_db(g_db);

    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);

    /* commit hash may be empty if repo is not a git repo, but should not crash */
    char *commit = cbm_artifact_commit(g_repo);
    /* For a non-git directory, commit will be NULL (git rev-parse HEAD fails) */
    free(commit);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_schema_version_mismatch) {
    setup_artifact_test();
    create_test_db(g_db);
    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);

    /* Overwrite artifact.json with incompatible schema version */
    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/.code-cortex/artifact.json", g_repo);
    FILE *fp = fopen(meta, "w");
    ASSERT_NOT_NULL(fp);
    fprintf(fp, "{\"schema_version\": 999, \"original_size\": 1000}");
    fclose(fp);

    /* exists should return false for incompatible version */
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    /* Import should fail */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    int rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_NEQ(rc, 0);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_import_missing) {
    setup_artifact_test();

    /* Import from repo without artifact should fail gracefully */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    int rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_NEQ(rc, 0);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_gitattributes_created) {
    setup_artifact_test();
    create_test_db(g_db);

    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);

    char ga[1024];
    snprintf(ga, sizeof(ga), "%s/.code-cortex/.gitattributes", g_repo);
    struct stat st;
    ASSERT_EQ(stat(ga, &st), 0);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_null_safety) {
    ASSERT_NEQ(cbm_artifact_export(NULL, "/tmp", "p", 0), 0);
    ASSERT_NEQ(cbm_artifact_export("/tmp/x.db", NULL, "p", 0), 0);
    ASSERT_NEQ(cbm_artifact_import(NULL, "/tmp/x.db"), 0);
    ASSERT_NEQ(cbm_artifact_import("/tmp", NULL), 0);
    ASSERT_FALSE(cbm_artifact_exists(NULL));
    ASSERT_NULL(cbm_artifact_commit(NULL));
    PASS();
}

/* #895: the FAST export path (watcher/incremental auto-update) read the
 * raw main-file bytes of a live WAL-mode store — committed rows still in
 * the -wal were missing and mid-checkpoint reads produced torn snapshots
 * that imported as page-corrupted caches. Export must snapshot
 * consistently (VACUUM INTO) on BOTH quality levels. */
TEST(artifact_fast_export_snapshots_live_wal_store) {
    setup_artifact_test();
    enum { WAL_NODES = 60 };

    /* Live store: rows committed but NOT checkpointed into the main file —
     * exactly the state the watcher export runs against. */
    cbm_store_t *s = cbm_store_open_path(g_db);
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "test-proj", "/tmp/test");
    for (int i = 0; i < WAL_NODES; i++) {
        char name[64];
        char qn[128];
        snprintf(name, sizeof(name), "walnode_%03d", i);
        snprintf(qn, sizeof(qn), "test-proj.mod.%s", name);
        cbm_node_t n = {.project = "test-proj",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "mod.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }

    /* Export WHILE the writer connection is still open. */
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    cbm_store_close(s);

    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported_wal.db", g_tmpdir);
    ASSERT_EQ(cbm_artifact_import(g_repo, import_db), 0);

    cbm_store_t *imp = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(imp);
    /* Torn snapshot = the WAL-resident rows are missing. */
    ASSERT_EQ(cbm_store_count_nodes(imp, "test-proj"), WAL_NODES);
    cbm_store_close(imp);
    PASS();
}

/* #895 (import half): page-level corruption must be refused at import.
 * The shallow integrity check only sanity-checks the projects table; the
 * deep variant runs PRAGMA quick_check and catches corrupt pages. */
TEST(store_deep_integrity_detects_page_corruption) {
    setup_artifact_test();
    enum { DEEP_NODES = 800, PAGE = 4096, ZERO_PAGES = 10 };
    char db2[1024];
    snprintf(db2, sizeof(db2), "%s/deep.db", g_tmpdir);
    cbm_store_t *s = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "deep", "/tmp/deep");
    for (int i = 0; i < DEEP_NODES; i++) {
        char name[64];
        char qn[192];
        snprintf(name, sizeof(name), "deep_probe_%04d", i);
        snprintf(qn, sizeof(qn), "deep.rather.long.module.path.for.page.fill.%s_pad_pad_pad", name);
        cbm_node_t n = {.project = "deep",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "deep.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }
    cbm_store_close(s);

    /* Healthy file passes the deep check. */
    cbm_store_t *ok = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(ok);
    ASSERT_TRUE(cbm_store_check_integrity_deep(ok));
    cbm_store_close(ok);

    /* Zero a mid-file band and the deep check must refuse. */
    FILE *f = fopen(db2, "rb+");
    ASSERT_NOT_NULL(f);
    (void)fseek(f, 0, SEEK_END);
    long pages = ftell(f) / PAGE;
    ASSERT_TRUE(pages > ZERO_PAGES + 6);
    char zero[PAGE];
    memset(zero, 0, sizeof(zero));
    (void)fseek(f, (pages / 2) * (long)PAGE, SEEK_SET);
    for (int i = 0; i < ZERO_PAGES; i++) {
        ASSERT_EQ(fwrite(zero, 1, PAGE, f), (size_t)PAGE);
    }
    (void)fclose(f);

    cbm_store_t *bad = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(bad);
    ASSERT_FALSE(cbm_store_check_integrity_deep(bad));
    cbm_store_close(bad);
    PASS();
}

/* Read a small file through the UTF-8 path layer (wide on Windows), so a
 * non-ASCII path is never misread by the test's own ANSI CRT call. Returns the
 * byte count, or 0 when the file cannot be opened or is empty. */
static size_t read_small_file_utf8(const char *path, char *buf, size_t cap) {
    buf[0] = '\0';
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    size_t rd = fread(buf, 1, cap - 1, fp);
    (void)fclose(fp);
    buf[rd] = '\0';
    return rd;
}

/* Upstream 76589541 + 402dd7c5 (#1171): a repository (and a cache directory)
 * under a non-ASCII path must export, report the artifact present, and import
 * — also onto an existing cache db. On Windows the atomic writer used the ANSI
 * CRT fopen + MoveFileExA, the existence check the ANSI stat, and
 * ensure_gitattributes the ANSI open(), so CJK characters outside the active
 * code page made every step fail (the .gitattributes merge=ours protection
 * went silently missing). The bytes are escaped UTF-8 for a CJK repo directory
 * name and a CJK cache directory name (kept ASCII in source). */
#define ART_CJK_REPO "\xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96\xe5\xa4\x8d\xe7\x8e\xb0"
#define ART_CJK_CACHE "\xe5\xaf\xbc\xe5\x85\xa5"
TEST(artifact_roundtrip_non_ascii_paths) {
    setup_artifact_test();
    create_test_db(g_db);

    char repo[1024];
    snprintf(repo, sizeof(repo), "%s/cbm-" ART_CJK_REPO, g_tmpdir);
    ASSERT_TRUE(cbm_mkdir_p(repo, 0755));

    ASSERT_EQ(cbm_artifact_export(g_db, repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    char zst[1024];
    snprintf(zst, sizeof(zst), "%s/" CBM_ARTIFACT_DIR "/" CBM_ARTIFACT_FILENAME, repo);
    ASSERT_TRUE(cbm_file_exists(zst));
    ASSERT_TRUE(cbm_artifact_exists(repo));

    char ga[1024];
    snprintf(ga, sizeof(ga), "%s/" CBM_ARTIFACT_DIR "/.gitattributes", repo);
    char ga_content[512];
    ASSERT_TRUE(read_small_file_utf8(ga, ga_content, sizeof(ga_content)) > 0);
    ASSERT_NOT_NULL(strstr(ga_content, CBM_ARTIFACT_FILENAME " binary merge=ours"));

    /* Create-only-if-absent: a re-export never rewrites an existing
     * .gitattributes the user may have edited. */
    static const char user_ga[] = "# user-owned\n";
    FILE *uga = cbm_fopen(ga, "wb");
    ASSERT_NOT_NULL(uga);
    ASSERT_TRUE(fputs(user_ga, uga) >= 0);
    ASSERT_EQ(fclose(uga), 0);
    ASSERT_EQ(cbm_artifact_export(g_db, repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    ASSERT_EQ(read_small_file_utf8(ga, ga_content, sizeof(ga_content)), strlen(user_ga));
    ASSERT_STR_EQ(ga_content, user_ga);

    char cache_dir[1024];
    snprintf(cache_dir, sizeof(cache_dir), "%s/" ART_CJK_CACHE, g_tmpdir);
    ASSERT_TRUE(cbm_mkdir_p(cache_dir, 0755));
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", cache_dir);
    ASSERT_EQ(cbm_artifact_import(repo, import_db), 0);
    /* Re-import replaces the existing cache db (the second-session path). */
    ASSERT_EQ(cbm_artifact_import(repo, import_db), 0);

    cbm_store_t *s = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_count_nodes(s, "test-proj"), 2);
    ASSERT_EQ(cbm_store_count_edges(s, "test-proj"), 1);
    cbm_store_close(s);

    cleanup_dir(g_tmpdir);
    PASS();
}

SUITE(artifact) {
    RUN_TEST(artifact_fast_export_snapshots_live_wal_store);
    RUN_TEST(store_deep_integrity_detects_page_corruption);
    RUN_TEST(artifact_concurrent_exports_do_not_collide);
    RUN_TEST(artifact_concurrent_exports_one_repo_publish_one_pair);
    RUN_TEST(artifact_export_fails_closed_without_publish_lock);
    RUN_TEST(artifact_export_removes_orphaned_snapshots);
    RUN_TEST(artifact_export_fast_roundtrip);
    RUN_TEST(artifact_export_best_roundtrip);
    RUN_TEST(artifact_exists_check);
    RUN_TEST(artifact_commit_hash);
    RUN_TEST(artifact_schema_version_mismatch);
    RUN_TEST(artifact_import_missing);
    RUN_TEST(artifact_gitattributes_created);
    RUN_TEST(artifact_null_safety);
    RUN_TEST(artifact_roundtrip_non_ascii_paths);
}
