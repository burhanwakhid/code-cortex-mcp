/*
 * test_store_checkpoint.c — Tests for WAL checkpoint behavior.
 *
 * Verifies that cbm_store_checkpoint() does not truncate the on-disk
 * WAL file. SQLITE_CHECKPOINT_TRUNCATE shrinks the WAL via ftruncate(fd, 0)
 * on success; on macOS this can raise SIGBUS in a sibling process that
 * has the DB mmap'd through SQLite when it next faults a page in the
 * now-shorter region. SQLITE_CHECKPOINT_PASSIVE marks frames as
 * checkpointed in the WAL header without changing the file size — disk
 * space is reclaimed on the next write cycle, not on every checkpoint.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <foundation/constants.h>
#include <foundation/platform.h>
#include <store/store.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

TEST(checkpoint_does_not_truncate_wal) {
    enum { N_ROWS = 100, PATH_BUF = 256, PATH_BUF_EXT = 300 };
    char db_path[PATH_BUF];
    snprintf(db_path, sizeof(db_path), "%s/cbm_test_ckpt_%d.db", cbm_tmpdir(), (int)getpid());
    char wal_path[PATH_BUF_EXT];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    char shm_path[PATH_BUF_EXT];
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
    unlink(db_path);
    unlink(wal_path);
    unlink(shm_path);

    cbm_store_t *s = cbm_store_open_path(db_path);
    ASSERT(s != NULL);

    /* Grow WAL beyond zero bytes via direct SQL. */
    int rc_sql = cbm_store_exec(
        s,
        "INSERT OR IGNORE INTO projects(name, indexed_at, root_path) "
        "VALUES('p', '2026-01-01', '/tmp/p');");
    ASSERT_EQ(rc_sql, 0);
    for (int i = 0; i < N_ROWS; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO nodes(project, label, name, qualified_name, file_path) "
                 "VALUES('p', 'Function', 'fn', 'p.module.fn_%d', 'f.c');",
                 i);
        rc_sql = cbm_store_exec(s, sql);
        ASSERT_EQ(rc_sql, 0);
    }

    /* WAL must exist and be non-empty before the checkpoint call. */
    struct stat st_before;
    int rc_stat = stat(wal_path, &st_before);
    ASSERT_EQ(rc_stat, 0);
    ASSERT(st_before.st_size > 0);

    /* Under SQLITE_CHECKPOINT_TRUNCATE the WAL would be ftruncate()d to 0
     * bytes on success. Under SQLITE_CHECKPOINT_PASSIVE the file size is
     * preserved (frames marked, not removed). */
    int rc_ckpt = cbm_store_checkpoint(s);
    ASSERT_EQ(rc_ckpt, 0); /* CBM_STORE_OK */

    struct stat st_after;
    rc_stat = stat(wal_path, &st_after);
    ASSERT_EQ(rc_stat, 0);
    ASSERT(st_after.st_size > 0);

    cbm_store_close(s);
    unlink(db_path);
    unlink(wal_path);
    unlink(shm_path);
    PASS();
}


/* #897: any code path installing a fresh DB file must delete the
 * destination's -wal/-shm first. SQLite decides whether to replay a WAL
 * purely from the sidecar's own header/checksums — a leftover WAL from a
 * crashed previous session is recovered ON TOP of the freshly installed
 * file at the next open, splicing old-generation pages into it (short
 * indexes, btreeInitPage failures, or resurrected stale rows).
 *
 * Repro (per the issue): hot-copy a live WAL aside, close cleanly, restore
 * the copy as the crashed-session leftover, install a fresh generation via
 * cbm_store_dump_to_file, reopen — the stale generation's row must NOT be
 * visible and the fresh row must be. */
static int tsc_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        (void)fclose(in);
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            (void)fclose(in);
            (void)fclose(out);
            return -1;
        }
    }
    (void)fclose(in);
    (void)fclose(out);
    return 0;
}

TEST(dump_install_ignores_stale_wal_sidecar) {
    char *td = th_mktempdir("cbm_stalewal");
    char db_path[512];
    char wal_path[512];
    char stale_copy[512];
    snprintf(db_path, sizeof(db_path), "%s/gen.db", td);
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(stale_copy, sizeof(stale_copy), "%s/stale.wal", td);

    /* Generation 1: file-backed store with a marker row living in the WAL. */
    cbm_store_t *s1 = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s1);
    cbm_store_upsert_project(s1, "walgen", "/tmp/walgen");
    cbm_node_t stale = {.project = "walgen",
                        .label = "Function",
                        .name = "stale_gen_node",
                        .qualified_name = "walgen.mod.stale_gen_node",
                        .file_path = "mod.py",
                        .start_line = 1,
                        .end_line = 2};
    ASSERT_TRUE(cbm_store_upsert_node(s1, &stale) > 0);

    /* Hot-copy the live WAL (must be non-empty or the repro is vacuous). */
    struct stat st_wal = {0};
    ASSERT_EQ(stat(wal_path, &st_wal), 0);
    ASSERT_TRUE(st_wal.st_size > 0);
    ASSERT_EQ(tsc_copy_file(wal_path, stale_copy), 0);
    cbm_store_close(s1); /* clean close checkpoints + removes the WAL */

    /* Simulate the crashed previous session's leftover sidecar. */
    ASSERT_EQ(tsc_copy_file(stale_copy, wal_path), 0);

    /* Generation 2: fresh store installed over db_path. */
    cbm_store_t *s2 = cbm_store_open_memory();
    ASSERT_NOT_NULL(s2);
    cbm_store_upsert_project(s2, "walgen", "/tmp/walgen");
    cbm_node_t fresh = {.project = "walgen",
                        .label = "Function",
                        .name = "fresh_gen_node",
                        .qualified_name = "walgen.mod.fresh_gen_node",
                        .file_path = "mod.py",
                        .start_line = 1,
                        .end_line = 2};
    ASSERT_TRUE(cbm_store_upsert_node(s2, &fresh) > 0);
    ASSERT_EQ(cbm_store_dump_to_file(s2, db_path), CBM_STORE_OK);
    cbm_store_close(s2);

    /* Reader: the stale WAL must not have been replayed onto gen 2. */
    cbm_store_t *s3 = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s3);
    cbm_node_t *hits = NULL;
    int hit_count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_name(s3, "walgen", "fresh_gen_node", &hits, &hit_count),
              CBM_STORE_OK);
    ASSERT_TRUE(hit_count >= 1);
    cbm_store_free_nodes(hits, hit_count);
    hits = NULL;
    hit_count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_name(s3, "walgen", "stale_gen_node", &hits, &hit_count),
              CBM_STORE_OK);
    ASSERT_EQ(hit_count, 0);
    cbm_store_free_nodes(hits, hit_count);
    cbm_store_close(s3);

    unlink(wal_path);
    char shm_path[512];
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
    unlink(shm_path);
    unlink(db_path);
    unlink(stale_copy);
    PASS();
}

#ifndef _WIN32
/* A path of exactly `len` bytes under base: 100-byte directories (created),
 * then a file name that makes up the rest. False when it cannot be built. */
static bool tsc_long_path(const char *base, size_t len, char *out, size_t outsz) {
    size_t n = (size_t)snprintf(out, outsz, "%s", base);
    while (len > n + 1 + 150) {
        char seg[101];
        memset(seg, 'd', 100);
        seg[100] = '\0';
        n += (size_t)snprintf(out + n, outsz - n, "/%s", seg);
        if (!cbm_mkdir_p(out, 0755)) {
            return false;
        }
    }
    size_t name = len - n - 1;
    if (name < 4 || n + 1 + name >= outsz) {
        return false;
    }
    out[n++] = '/';
    memset(out + n, 'f', name - 3);
    memcpy(out + n + name - 3, ".db", 4);
    return strlen(out) == len;
}
#endif

/* A destination whose temp name ("<dest>.tmp") does not fit the path buffer
 * is refused before anything is touched: cut short, the temp name was the
 * destination itself, which was unlinked first. One that fits is dumped. */
TEST(dump_refuses_destination_too_long_for_temp) {
#ifndef _WIN32
    char *td = th_mktempdir("cbm_dumplong");
    ASSERT_NOT_NULL(td);
    char base[256];
    snprintf(base, sizeof(base), "%s", td);
#ifndef _WIN32
    /* The resolved form: on macOS /tmp is /private/tmp, and a path built to
     * the limit from the shorter spelling exceeds it once resolved. */
    char real[PATH_MAX];
    if (realpath(td, real)) {
        snprintf(base, sizeof(base), "%s", real);
    }
#endif
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "longp", "/tmp/longp"), CBM_STORE_OK);

    /* (SQLite's own path limit, 512 bytes on POSIX, bounds what can be
     * dumped at all; the temp name's truncation was the hazard past it.) */
    static char fits[CBM_PATH_MAX + 64];
    ASSERT_TRUE(tsc_long_path(base, 400, fits, sizeof(fits)));
    ASSERT_EQ(cbm_store_dump_to_file(s, fits), CBM_STORE_OK);
    ASSERT_TRUE(cbm_file_exists(fits));

    static char too_long[CBM_PATH_MAX + 64];
    ASSERT_TRUE(tsc_long_path(base, CBM_PATH_MAX - 1, too_long, sizeof(too_long)));
    ASSERT_EQ(th_write_file(too_long, "old"), 0);
    ASSERT_EQ(cbm_store_dump_to_file(s, too_long), CBM_STORE_ERR);
    char buf[16] = {0};
    FILE *f = fopen(too_long, "rb");
    ASSERT_NOT_NULL(f);
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    ASSERT_EQ((int)got, 3);
    ASSERT_STR_EQ(buf, "old"); /* untouched */
    cbm_store_close(s);
    th_rmtree(base);
#endif
    PASS();
}

SUITE(store_checkpoint) {
    RUN_TEST(checkpoint_does_not_truncate_wal);
    RUN_TEST(dump_install_ignores_stale_wal_sidecar);
    RUN_TEST(dump_refuses_destination_too_long_for_temp);
}
