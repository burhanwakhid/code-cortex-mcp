/*
 * test_store_pragmas.c — Tests for SQLite pragma resolution.
 *
 * Validates that the CBM_SQLITE_MMAP_SIZE env var controls the mmap_size
 * pragma applied to on-disk stores. Default behavior (env unset) must
 * remain 64 MB. Setting the env to 0 disables memory-mapped I/O so
 * concurrent processes that truncate the DB file under a sibling's live
 * mapping return SQLITE_IOERR instead of crashing the process with SIGBUS.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <store/store.h>
#include "sqlite3.h" /* vendored/sqlite3: read pragmas back on the store's own handle */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clear_mmap_env(void) {
    cbm_unsetenv("CBM_SQLITE_MMAP_SIZE");
}

TEST(mmap_size_default_when_unset) {
    clear_mmap_env();
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    PASS();
}

TEST(mmap_size_zero_disables_mmap) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_explicit_value) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "1048576", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 1048576LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_negative_clamped_to_zero) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "-1", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "not-a-number", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_partial_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "123abc", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

/* Integration smoke: opening a file-backed store with mmap_size=0 must
 * succeed. Proves the resolver is wired through configure_pragmas(). */
TEST(store_open_with_mmap_disabled) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_pragmas_%d.db", cbm_tmpdir(), (int)getpid());
    unlink(tmp_path);

    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    cbm_store_close(s);

    unlink(tmp_path);
    /* WAL/SHM siblings created by the open */
    char tmp_wal[300];
    char tmp_shm[300];
    snprintf(tmp_wal, sizeof(tmp_wal), "%s-wal", tmp_path);
    snprintf(tmp_shm, sizeof(tmp_shm), "%s-shm", tmp_path);
    unlink(tmp_wal);
    unlink(tmp_shm);

    clear_mmap_env();
    PASS();
}

/* #1083: the WAL bound on write connections. */
TEST(journal_size_limit_default_and_env) {
    cbm_unsetenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT");
    ASSERT_EQ(cbm_store_resolve_journal_size_limit(), 268435456LL);
    cbm_setenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT", "1048576", 1);
    ASSERT_EQ(cbm_store_resolve_journal_size_limit(), 1048576LL);
    cbm_setenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT", "-5", 1);
    ASSERT_EQ(cbm_store_resolve_journal_size_limit(), -1LL);
    cbm_setenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT", "12x", 1);
    ASSERT_EQ(cbm_store_resolve_journal_size_limit(), 268435456LL);
    cbm_unsetenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT");
    PASS();
}

static void jsl_unlink_all(const char *path) {
    char side[300];
    unlink(path);
    snprintf(side, sizeof(side), "%s-wal", path);
    unlink(side);
    snprintf(side, sizeof(side), "%s-shm", path);
    unlink(side);
}

static long long jsl_wal_size(const char *path) {
    char wal[300];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    FILE *f = fopen(wal, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

/* A write connection reports the limit, and a WAL that grew past it is cut
 * back at the first write after a checkpoint that copied every frame. */
TEST(journal_size_limit_trims_wal_after_checkpoint) {
    cbm_unsetenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT");
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_jsl_%d.db", cbm_tmpdir(), (int)getpid());
    jsl_unlink_all(tmp_path);
    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    ASSERT_EQ(cbm_store_journal_size_limit(s), 268435456LL);
    cbm_store_close(s);
    jsl_unlink_all(tmp_path);

    enum { LIMIT = 1048576 };
    cbm_setenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT", "1048576", 1);
    s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    ASSERT_EQ(cbm_store_journal_size_limit(s), (long long)LIMIT);
    sqlite3 *db = cbm_store_get_db(s);
    ASSERT_EQ(sqlite3_exec(db,
                           "CREATE TABLE big(b BLOB); BEGIN;"
                           "INSERT INTO big SELECT zeroblob(1048576) FROM "
                           "(SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 "
                           "UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL "
                           "SELECT 8); COMMIT;",
                           NULL, NULL, NULL),
              SQLITE_OK);
    ASSERT_GT(jsl_wal_size(tmp_path), 4LL * LIMIT);
    int frames = -1;
    int copied = -1;
    ASSERT_EQ(sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &frames, &copied),
              SQLITE_OK);
    ASSERT_EQ(frames, copied);
    ASSERT_EQ(sqlite3_exec(db, "DELETE FROM big WHERE rowid = 1;", NULL, NULL, NULL), SQLITE_OK);
    ASSERT_LTE(jsl_wal_size(tmp_path), (long long)LIMIT);
    cbm_store_close(s);
    jsl_unlink_all(tmp_path);
    cbm_unsetenv("CBM_SQLITE_JOURNAL_SIZE_LIMIT");
    PASS();
}


/* #896: a row-scan that dies mid-stream (SQLITE_CORRUPT) must surface a
 * loud store error, not masquerade as a clean end of results. Counts are
 * answered from covering indexes (still correct) while row fetches die at
 * the first corrupt table page — the old loops discarded the terminal
 * sqlite3_step code, so every query surface returned plausible
 * truncated/empty answers with no error. */
TEST(corrupt_page_scan_returns_error_not_truncation) {
    enum { CORRUPT_NODES = 2000, ZERO_PAGES = 40 };
    char *td = th_mktempdir("cbm_corrupt");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/c.db", td);

    cbm_store_t *s = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "corr", "/tmp/corr");
    for (int i = 0; i < CORRUPT_NODES; i++) {
        char name[64];
        char qn[256];
        snprintf(name, sizeof(name), "corrupt_probe_fn_%04d", i);
        snprintf(qn, sizeof(qn),
                 "corr.some.rather.long.module.path.to.fill.table.pages.%s_padding_padding",
                 name);
        cbm_node_t n = {.project = "corr",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "src/corrupt_probe.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }
    /* Precondition: a full scan works on the healthy file. */
    cbm_search_params_t params = {.project = "corr", .label = "Function", .limit = 50};
    cbm_search_output_t out = {0};
    ASSERT_EQ(cbm_store_search(s, &params, &out), CBM_STORE_OK);
    ASSERT_EQ(out.total, CORRUPT_NODES);
    cbm_store_search_free(&out);
    cbm_store_close(s);

    /* Zero a band of mid-file pages (the report's dd repro): page 25%..
     * covers nodes-table leaves on a file this shape. */
    FILE *f = fopen(db_path, "rb+");
    ASSERT_NOT_NULL(f);
    (void)fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    enum { PAGE = 4096 };
    long page_count = fsize / PAGE;
    ASSERT_TRUE(page_count > ZERO_PAGES + 8);
    char zero[PAGE];
    memset(zero, 0, sizeof(zero));
    (void)fseek(f, (page_count / 4) * (long)PAGE, SEEK_SET);
    for (int i = 0; i < ZERO_PAGES; i++) {
        ASSERT_EQ(fwrite(zero, 1, PAGE, f), (size_t)PAGE);
    }
    (void)fclose(f);

    /* The scans must now fail LOUDLY (CBM_STORE_ERR), not truncate. */
    cbm_store_t *s2 = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s2);
    /* The scan must CROSS the corrupt band: request every row. */
    cbm_search_params_t all_params = {.project = "corr", .label = "Function",
                                      .limit = CORRUPT_NODES};
    cbm_search_output_t out2 = {0};
    int rc_search = cbm_store_search(s2, &all_params, &out2);
    if (rc_search == CBM_STORE_OK && out2.count == CORRUPT_NODES) {
        /* Vacuous-guard: a complete, healthy scan means corruption missed
         * the table pages — rebuild the fixture, don't relax the assert. */
        FAIL("fixture failed to hit table pages (full scan healthy)");
    }
    /* THE BUG (#896): OK + silently truncated rows. Fixed = loud ERR. */
    ASSERT_EQ(rc_search, CBM_STORE_ERR);
    cbm_store_search_free(&out2);

    /* Point lookups may legitimately succeed when their row's page
     * escaped the corrupt band — the class contract is about SCANS. A
     * second scan surface (qn-suffix, different SQL path) must also err. */
    cbm_node_t *hits = NULL;
    int hit_count = 0;
    int rc_suffix =
        cbm_store_find_nodes_by_qn_suffix(s2, "corr", "padding_padding", &hits, &hit_count);
    if (rc_suffix == CBM_STORE_OK && hit_count == CORRUPT_NODES) {
        FAIL("suffix scan healthy — fixture failed to hit table pages");
    }
    ASSERT_EQ(rc_suffix, CBM_STORE_ERR);
    cbm_store_free_nodes(hits, hit_count);
    cbm_store_close(s2);

    unlink(db_path);
    PASS();
}

/* A read tool's statements share one read batch instead of taking the file
 * lock once per statement. The batch reads one snapshot until it renews
 * (every 100 ms, forced here through the seam), then sees later commits. */
static char *read_batch_name(cbm_store_t *s) {
    cbm_node_t found = {0};
    if (cbm_store_find_node_by_qn(s, "p", "p.a", &found) != CBM_STORE_OK) {
        return NULL;
    }
    char *name = found.name ? strdup(found.name) : NULL;
    cbm_node_free_fields(&found);
    return name;
}

static bool read_batch_rename(cbm_store_t *writer, const char *name) {
    cbm_node_t node = {
        .project = "p", .label = "Function", .name = name, .qualified_name = "p.a", .file_path = "a.c"};
    return cbm_store_upsert_node(writer, &node) > 0;
}

TEST(store_read_batch_snapshot_renews) {
    char *td = th_mktempdir("cbm_read_batch");
    ASSERT_NOT_NULL(td);
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/batch.db", td);
    cbm_store_t *writer = cbm_store_open_path(db_path); /* WAL: commits beside a reader */
    ASSERT_NOT_NULL(writer);
    ASSERT_EQ(cbm_store_upsert_project(writer, "p", td), CBM_STORE_OK);
    ASSERT_TRUE(read_batch_rename(writer, "before"));
    cbm_store_t *reader = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(reader);

    ASSERT_TRUE(cbm_store_read_batch_begin(reader));
    ASSERT_FALSE(cbm_store_read_batch_begin(reader)); /* one batch at a time */
    char *name = read_batch_name(reader);
    ASSERT_STR_EQ(name, "before");
    free(name);

    /* Inside the batch the reader keeps its snapshot... */
    ASSERT_TRUE(read_batch_rename(writer, "after"));
    name = read_batch_name(reader);
    ASSERT_STR_EQ(name, "before");
    free(name);

    /* ...until the batch renews at its next statement. */
    cbm_store_read_batch_expire_for_test(reader);
    name = read_batch_name(reader);
    ASSERT_STR_EQ(name, "after");
    free(name);

    /* Ended, every statement reads the latest commit again. */
    cbm_store_read_batch_end(reader);
    ASSERT_TRUE(read_batch_rename(writer, "latest"));
    name = read_batch_name(reader);
    ASSERT_STR_EQ(name, "latest");
    free(name);

    /* Begin refuses while another transaction is open; close ends a batch. */
    ASSERT_EQ(cbm_store_begin(writer), CBM_STORE_OK);
    ASSERT_FALSE(cbm_store_read_batch_begin(writer));
    ASSERT_EQ(cbm_store_rollback(writer), CBM_STORE_OK);
    ASSERT_TRUE(cbm_store_read_batch_begin(reader));
    cbm_store_close(reader);
    cbm_store_close(writer);
    th_rmtree(td);
    PASS();
}

/* #1419: effective pragmas per store role, read on the handle itself —
 * synchronous is per-connection and never persisted, so an external sqlite3
 * shell reports its own default (FULL), not what the indexer runs with. */
static int role_pragma_int(cbm_store_t *s, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(cbm_store_get_db(s), sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

TEST(store_role_pragmas_issue1419) {
    enum {
        SYNC_OFF = 0,
        SYNC_NORMAL = 1,
        /* 64 MiB = 1024 of the 64 KiB pages every index is written with. */
        WRITE_CACHE_KIB = -65536,
        SQLITE_DEFAULT_CACHE_KIB = -2000,
    };
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_roles_%d.db", cbm_tmpdir(), (int)getpid());
    unlink(tmp_path);

    /* Read-write role: WAL at NORMAL, with a page cache that holds more than a
     * few 64 KiB pages. */
    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    ASSERT_EQ(role_pragma_int(s, "PRAGMA synchronous;"), SYNC_NORMAL);
    ASSERT_EQ(role_pragma_int(s, "PRAGMA cache_size;"), WRITE_CACHE_KIB);
    /* Bulk role: sync off for the write burst, then back to the read-write
     * settings rather than SQLite's defaults. */
    ASSERT_EQ(cbm_store_begin_bulk(s), CBM_STORE_OK);
    ASSERT_EQ(role_pragma_int(s, "PRAGMA synchronous;"), SYNC_OFF);
    ASSERT_EQ(role_pragma_int(s, "PRAGMA cache_size;"), WRITE_CACHE_KIB);
    ASSERT_EQ(cbm_store_end_bulk(s), CBM_STORE_OK);
    ASSERT_EQ(role_pragma_int(s, "PRAGMA synchronous;"), SYNC_NORMAL);
    ASSERT_EQ(role_pragma_int(s, "PRAGMA cache_size;"), WRITE_CACHE_KIB);
    cbm_store_close(s);

    /* Query role: read-only, keeps SQLite's small default cache (MCP read
     * tools open one per project, so it stays cheap). */
    cbm_store_t *q = cbm_store_open_path_query(tmp_path);
    ASSERT(q != NULL);
    ASSERT_EQ(role_pragma_int(q, "PRAGMA cache_size;"), SQLITE_DEFAULT_CACHE_KIB);
    cbm_store_close(q);

    unlink(tmp_path);
    char tmp_wal[300];
    char tmp_shm[300];
    snprintf(tmp_wal, sizeof(tmp_wal), "%s-wal", tmp_path);
    snprintf(tmp_shm, sizeof(tmp_shm), "%s-shm", tmp_path);
    unlink(tmp_wal);
    unlink(tmp_shm);
    PASS();
}

SUITE(store_pragmas) {
    RUN_TEST(corrupt_page_scan_returns_error_not_truncation);
    RUN_TEST(mmap_size_default_when_unset);
    RUN_TEST(mmap_size_zero_disables_mmap);
    RUN_TEST(mmap_size_explicit_value);
    RUN_TEST(mmap_size_negative_clamped_to_zero);
    RUN_TEST(mmap_size_garbage_falls_back_to_default);
    RUN_TEST(mmap_size_partial_garbage_falls_back_to_default);
    RUN_TEST(store_open_with_mmap_disabled);
    RUN_TEST(store_read_batch_snapshot_renews);
    RUN_TEST(store_role_pragmas_issue1419);
    RUN_TEST(journal_size_limit_default_and_env);
    RUN_TEST(journal_size_limit_trims_wal_after_checkpoint);
}
