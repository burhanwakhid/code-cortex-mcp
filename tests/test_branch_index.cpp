/*
 * test_branch_index.cpp — One index per git branch (pipeline/branch_slots.h).
 *
 * Builds a two-branch scratch repo, indexes it through the default cache
 * database, and switches branches between runs: the outgoing branch's
 * database must be parked, the incoming one restored (without re-extracting
 * files git rewrote to the same bytes), parked files must stay invisible to
 * list_projects, delete_project must remove them, the cap must evict, and the
 * off switch must bring back the single in-place database.
 *
 * The suite's HOME is a temp dir (test_main.cpp), so the cache is too.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <foundation/compat_fs.h>
#include <git/git_context.h>
#include <foundation/git_env.h>
#include <mcp/mcp.h>
#include <pipeline/artifact.h>
#include <pipeline/branch_slots.h>
#include <pipeline/pipeline.h>
#include <pipeline/pipeline_internal.h> /* cbm_pipeline_test_set_brief_recompute_max */
#include <store/store.h>
#include <foundation/constants.h>
#include <foundation/platform.h>
#include <foundation/sha256.h>

#include <ctype.h>
#include <stdio.h>
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static const char *bi_null_dev(void) {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

/* core.longpaths: the long-name tests create loose refs past MAX_PATH, which
 * Git for Windows refuses unless told to use the long-path API. */
static int bi_git(const char *repo, const char *args) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" -c user.name=t -c user.email=t@t.io -c commit.gpgsign=false "
             "-c init.defaultBranch=main -c core.longpaths=true %s >%s 2>&1",
             repo, args, bi_null_dev());
    return system(cmd);
}

/* main: src/a.py (only_main) + src/common.py; feature: a.py removed,
 * src/b.py (only_feature) added. Returns the repo path (static) or NULL. */
static char *bi_make_repo(const char *prefix) {
    char *tmp = th_mktempdir(prefix);
    if (!tmp || bi_git(tmp, "init -q -b main") != 0) {
        return NULL;
    }
    th_write_file(TH_PATH(tmp, "src/a.py"),
                  "def shared():\n    return 1\n\ndef only_main():\n    return shared()\n");
    th_write_file(TH_PATH(tmp, "src/common.py"), "def common_helper():\n    return 2\n");
    if (bi_git(tmp, "add .") != 0 || bi_git(tmp, "commit -q -m main") != 0 ||
        bi_git(tmp, "checkout -q -b feature") != 0 || bi_git(tmp, "rm -q src/a.py") != 0) {
        return NULL;
    }
    th_write_file(TH_PATH(tmp, "src/b.py"),
                  "def shared():\n    return 1\n\ndef only_feature():\n    return shared()\n");
    if (bi_git(tmp, "add .") != 0 || bi_git(tmp, "commit -q -m feature") != 0 ||
        bi_git(tmp, "checkout -q main") != 0) {
        return NULL;
    }
    return tmp;
}

typedef struct {
    int rc;
    char action[32];
    char from[128];
    int reparsed;
} bi_run_t;

static char g_bi_project[1024];
static char g_bi_root[1024]; /* the checkout g_bi_project was last indexed from */

/* Index repo into the default cache database. keep < 0: pipeline default. */
static bi_run_t bi_index(const char *repo, int keep) {
    bi_run_t r = {CBM_NOT_FOUND, "", "", -1};
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    if (!p) {
        return r;
    }
    if (keep >= 0) {
        cbm_pipeline_set_branch_keep(p, keep);
    }
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    r.rc = cbm_pipeline_run(p);
    const char *action = NULL;
    const char *from = NULL;
    cbm_pipeline_get_branch_switch(p, &action, &from, NULL, &r.reparsed);
    snprintf(r.action, sizeof(r.action), "%s", action ? action : "");
    snprintf(r.from, sizeof(r.from), "%s", from ? from : "");
    cbm_pipeline_free(p);
    return r;
}

static const char *bi_db_path(char *buf, size_t n) {
    snprintf(buf, n, "%s/%s.db", cbm_resolve_cache_dir(), g_bi_project);
    return buf;
}

/* Count nodes named `name` in the project's current database. */
static int bi_count(const char *name) {
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    if (!s) {
        return -1;
    }
    cbm_node_t *nodes = NULL;
    int count = 0;
    cbm_store_find_nodes_by_name(s, g_bi_project, name, &nodes, &count);
    cbm_store_free_nodes(nodes, count);
    cbm_store_close(s);
    return count;
}

static int bi_parked(char ***out) {
    char db[1024];
    return cbm_branch_slots_list(bi_db_path(db, sizeof(db)), g_bi_project, g_bi_root, out);
}

static bool bi_is_parked(const char *slug) {
    char **list = NULL;
    int n = bi_parked(&list);
    bool hit = false;
    for (int i = 0; i < n; i++) {
        hit = hit || strcmp(list[i], slug) == 0;
    }
    cbm_branch_slots_free_list(list, n);
    return hit;
}

static void bi_cleanup(const char *repo) {
    char db[1024];
    bi_db_path(db, sizeof(db));
    (void)cbm_branch_slots_delete_all(db, g_bi_project);
    cbm_unlink(db);
    cbm_remove_db_sidecars(db);
    th_rmtree(repo);
}

/* GIT_CONFIG_COUNT and the first entries it names, saved and put back. */
enum { BI_GIT_ENV_N = 4 };
static const char *const BI_GIT_ENV[] = {
    "GIT_CONFIG_COUNT",   "GIT_CONFIG_KEY_0",   "GIT_CONFIG_VALUE_0", "GIT_CONFIG_KEY_1",
    "GIT_CONFIG_VALUE_1", "GIT_CONFIG_KEY_2",   "GIT_CONFIG_VALUE_2", "GIT_CONFIG_KEY_3",
    "GIT_CONFIG_VALUE_3"};
typedef struct {
    char val[BI_GIT_ENV_N * 2 + 1][256];
    bool had[BI_GIT_ENV_N * 2 + 1];
} bi_git_env_t;

static void bi_git_env_save(bi_git_env_t *s, bool clear) {
    for (size_t i = 0; i < sizeof(BI_GIT_ENV) / sizeof(BI_GIT_ENV[0]); i++) {
        s->had[i] = cbm_safe_getenv(BI_GIT_ENV[i], s->val[i], sizeof(s->val[i]), NULL) != NULL;
        if (clear) {
            cbm_unsetenv(BI_GIT_ENV[i]);
        }
    }
}

static void bi_git_env_restore(const bi_git_env_t *s) {
    for (size_t i = 0; i < sizeof(BI_GIT_ENV) / sizeof(BI_GIT_ENV[0]); i++) {
        if (s->had[i]) {
            cbm_setenv(BI_GIT_ENV[i], s->val[i], 1);
        } else {
            cbm_unsetenv(BI_GIT_ENV[i]);
        }
    }
}

static const char *bi_env(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}

/* The setting git runs with (core.longpaths on Windows) is appended to the
 * GIT_CONFIG_* set a parent exports, not dropped because one exists, and a
 * key the parent already set keeps the parent's value. */
TEST(branch_git_config_env_append) {
    bi_git_env_t saved;
    bi_git_env_save(&saved, true);

    ASSERT_TRUE(cbm_git_config_env_append("core.longpaths", "true"));
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_COUNT"), "1");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_KEY_0"), "core.longpaths");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_VALUE_0"), "true");

    cbm_setenv("GIT_CONFIG_COUNT", "2", 1);
    cbm_setenv("GIT_CONFIG_KEY_0", "user.name", 1);
    cbm_setenv("GIT_CONFIG_VALUE_0", "ci", 1);
    cbm_setenv("GIT_CONFIG_KEY_1", "safe.directory", 1);
    cbm_setenv("GIT_CONFIG_VALUE_1", "*", 1);
    ASSERT_TRUE(cbm_git_config_env_append("core.longpaths", "true"));
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_COUNT"), "3");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_KEY_0"), "user.name");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_KEY_1"), "safe.directory");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_KEY_2"), "core.longpaths");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_VALUE_2"), "true");

    /* Already present (any case): unchanged. */
    cbm_setenv("GIT_CONFIG_COUNT", "2", 1);
    cbm_setenv("GIT_CONFIG_KEY_1", "Core.LongPaths", 1);
    cbm_setenv("GIT_CONFIG_VALUE_1", "false", 1);
    cbm_unsetenv("GIT_CONFIG_KEY_2");
    cbm_unsetenv("GIT_CONFIG_VALUE_2");
    ASSERT_TRUE(cbm_git_config_env_append("core.longpaths", "true"));
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_COUNT"), "2");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_VALUE_1"), "false");
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_KEY_2"), "");

    /* A count git would reject: left alone. */
    cbm_setenv("GIT_CONFIG_COUNT", "two", 1);
    ASSERT_FALSE(cbm_git_config_env_append("core.longpaths", "true"));
    ASSERT_STR_EQ(bi_env("GIT_CONFIG_COUNT"), "two");

    bi_git_env_restore(&saved);
    PASS();
}

/* Distinct branch names never share a slug: one the slugger changed ("a/b"
 * against "a-b"), one with nothing it can keep (all non-ASCII), and the
 * reserved "detached" and "working-tree" get a hash of the whole name. A
 * name that is its own slug stays as it is. Every slug is one safe path
 * component, and long ones still map to bounded slot names. */
static bool bi_slug_safe(const char *s) {
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return s[0] != '\0';
}

TEST(branch_slugs_never_collide) {
    const char *names[] = {"a/b",          "a-b",      "a_b",          "-a-b",
                           "\xc3\xa9\xc3\xa9", "\xc3\xa8\xc3\xa8", "\xe4\xb8\xad", "detached",
                           "working-tree", "branch",   "feature/x/y",  "feature-x-y"};
    enum { N = sizeof(names) / sizeof(names[0]) };
    char *slugs[N + 2];
    for (int i = 0; i < N; i++) {
        slugs[i] = cbm_git_branch_slug(names[i], false);
        ASSERT_NOT_NULL(slugs[i]);
        ASSERT_TRUE(bi_slug_safe(slugs[i]));
    }
    slugs[N] = cbm_git_branch_slug(NULL, true);      /* every detached HEAD */
    slugs[N + 1] = cbm_git_branch_slug(NULL, false); /* outside git */
    ASSERT_STR_EQ(slugs[N], "detached");
    ASSERT_STR_EQ(slugs[N + 1], "working-tree");
    for (int i = 0; i < N + 2; i++) {
        for (int k = i + 1; k < N + 2; k++) {
            ASSERT_STR_NEQ(slugs[i], slugs[k]);
        }
    }
    ASSERT_STR_EQ(slugs[1], "a-b"); /* its own slug: unchanged */
    ASSERT_STR_EQ(slugs[9], "branch");
    ASSERT_EQ(strncmp(slugs[0], "a-b-", 4), 0);
    ASSERT_EQ((int)strlen(slugs[0]), 4 + 12);
    ASSERT_EQ(strncmp(slugs[4], "branch-", 7), 0);
    ASSERT_EQ(strncmp(slugs[7], "detached-", 9), 0);
    char *again = cbm_git_branch_slug("a/b", false);
    ASSERT_STR_EQ(again, slugs[0]); /* stable */
    free(again);
    for (int i = 0; i < N + 2; i++) {
        free(slugs[i]);
    }
    /* A long name the slugger changed: still one bounded slot name. */
    static char longname[600];
    memset(longname, 'q', sizeof(longname) - 1);
    longname[100] = '/';
    longname[sizeof(longname) - 1] = '\0';
    char *ls = cbm_git_branch_slug(longname, false);
    ASSERT_NOT_NULL(ls);
    char slot[CBM_BRANCH_SLOT_NAME_MAX + 1];
    ASSERT_TRUE(cbm_branch_slot_name(ls, slot, sizeof(slot)));
    ASSERT_EQ((int)strlen(slot), CBM_BRANCH_SLOT_NAME_MAX);
    free(ls);
    PASS();
}

TEST(branch_slug_from_qn) {
    ASSERT_STR_EQ(cbm_branch_slug_from_qn("proj.__branch__.main", "proj"), "main");
    /* A branch whose name contains the marker keeps all of it. */
    ASSERT_STR_EQ(cbm_branch_slug_from_qn("a.__branch__.b.__branch__.feature-x", "a"),
                  "b.__branch__.feature-x");
    ASSERT_NULL(cbm_branch_slug_from_qn("proj.main", "proj"));
    ASSERT_NULL(cbm_branch_slug_from_qn("proj.__branch__.", "proj"));
    ASSERT_NULL(cbm_branch_slug_from_qn("other.__branch__.main", "proj"));
    ASSERT_NULL(cbm_branch_slug_from_qn(NULL, "proj"));
    char dir[256];
    ASSERT_TRUE(cbm_branch_slots_dir("/c/proj.db", "proj", NULL, dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "/c/branches/proj");
    ASSERT_FALSE(cbm_branch_slots_dir("/c/proj.db", "../x", NULL, dir, sizeof(dir)));
    PASS();
}

/* Slug of the HEAD the file-only reader sees from dir; "" when it fails. */
static const char *bi_head_slug(const char *dir, char *buf, size_t n) {
    buf[0] = '\0';
    cbm_git_head_t head;
    if (cbm_git_head_read(dir, &head) == 0) {
        char *slug = cbm_git_branch_slug(head.branch, head.is_detached);
        snprintf(buf, n, "%s", slug ? slug : "");
        free(slug);
        cbm_git_head_free(&head);
    }
    return buf;
}

/* The subprocess-free HEAD read agrees with cbm_git_context_resolve, also
 * through a linked worktree's .git file, and from any directory inside the
 * checkout (an index root below the checkout root). */
TEST(branch_git_head_slug_reads_head_file) {
    char *repo = bi_make_repo("cbm_bi_head");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char slug[256];
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/src", repo);

    ASSERT_STR_EQ(bi_head_slug(repo, slug, sizeof(slug)), "main");
    ASSERT_STR_EQ(bi_head_slug(sub, slug, sizeof(slug)), "main");
    ASSERT_EQ(bi_git(repo, "checkout -q -b topic/x-y"), 0);
    char *want = cbm_git_branch_slug("topic/x-y", false);
    ASSERT_NOT_NULL(want);
    ASSERT_EQ(strncmp(want, "topic-x-y-", 10), 0); /* plus the name's hash */
    ASSERT_STR_EQ(bi_head_slug(repo, slug, sizeof(slug)), want);
    free(want);
    ASSERT_EQ(bi_git(repo, "checkout -q --detach HEAD"), 0);
    ASSERT_STR_EQ(bi_head_slug(repo, slug, sizeof(slug)), "detached");

    char wt[512];
    snprintf(wt, sizeof(wt), "%s/wt", repo);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "worktree add -q -b side \"%s\"", wt);
    ASSERT_EQ(bi_git(repo, cmd), 0);
    ASSERT_STR_EQ(bi_head_slug(wt, slug, sizeof(slug)), "side");
    char wt_sub[600];
    snprintf(wt_sub, sizeof(wt_sub), "%s/src", wt);
    ASSERT_STR_EQ(bi_head_slug(wt_sub, slug, sizeof(slug)), "side");
    cbm_git_context_t ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(wt, &ctx), 0);
    ASSERT_STR_EQ(ctx.branch_slug, "side");
    cbm_git_context_free(&ctx);

    /* Outside any checkout there is nothing to read. */
    char *plain = th_mktempdir("cbm_bi_nogit");
    ASSERT_NOT_NULL(plain);
    ASSERT_STR_EQ(bi_head_slug(plain, slug, sizeof(slug)), "");
    th_rmtree(plain);

    th_rmtree(repo);
    PASS();
}

/* A checkout under a non-ASCII directory: the walk up to .git and the HEAD
 * and ref reads take the UTF-8 path as is (wide on Windows). */
static bool bi_put(const char *path, const char *content) {
    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fputs(content, f) >= 0;
    return fclose(f) == 0 && ok;
}

TEST(branch_git_head_read_non_ascii_path) {
    char *tmp = th_mktempdir("cbm_bi_utf8");
    ASSERT_NOT_NULL(tmp);
    char base[512];
    snprintf(base, sizeof(base), "%s", tmp);
    char repo[700];
    char p[900];
    /* "répo-日本" */
    snprintf(repo, sizeof(repo), "%s/r\xc3\xa9po-\xe6\x97\xa5\xe6\x9c\xac", base);
    snprintf(p, sizeof(p), "%s/.git/refs/heads", repo);
    ASSERT_TRUE(cbm_mkdir_p(p, 0755));
    snprintf(p, sizeof(p), "%s/sub/\xc3\xbc", repo); /* an index root below the checkout */
    ASSERT_TRUE(cbm_mkdir_p(p, 0755));
    snprintf(p, sizeof(p), "%s/.git/HEAD", repo);
    ASSERT_TRUE(bi_put(p, "ref: refs/heads/main\n"));
    static const char sha[] = "0123456789abcdef0123456789abcdef01234567";
    snprintf(p, sizeof(p), "%s/.git/refs/heads/main", repo);
    ASSERT_TRUE(bi_put(p, "0123456789abcdef0123456789abcdef01234567\n"));
    snprintf(p, sizeof(p), "%s/sub/\xc3\xbc", repo);
    cbm_git_head_t head;
    ASSERT_EQ(cbm_git_head_read(p, &head), 0);
    ASSERT_STR_EQ(head.branch, "main");
    ASSERT_STR_EQ(head.head_sha, sha);
    ASSERT_FALSE(head.is_detached);
    cbm_git_head_free(&head);
    th_rmtree(base);
    PASS();
}

/* The file reader caps a line at 4 KiB: an over-long HEAD is not found
 * (no unbounded allocation), and an over-long packed-refs line is skipped
 * without hiding the branch's ref after it. */
/* A UNC path with no .git anywhere above it ends the walk: reduced to its
 * \\server stub and then a lone backslash, it used to be cut to itself and
 * probed forever. The walk treats backslashes as separators everywhere, so
 * the shape is exercised on every platform. */
TEST(branch_git_head_read_unc_without_git_ends) {
    cbm_git_head_t h;
    ASSERT_NEQ(cbm_git_head_read("\\\\cbm-nohost\\share\\repo", &h), 0);
    ASSERT_NEQ(cbm_git_head_read("\\\\cbm-nohost", &h), 0);
    ASSERT_NEQ(cbm_git_head_read("\\", &h), 0);
    PASS();
}

#ifdef _WIN32
/* Every git child on Windows runs with core.longpaths on: the scrubbed
 * environment block carries it as its only GIT_CONFIG_* set, whatever the
 * parent exported (a parent's GIT_CONFIG_COUNT and items are dropped). */
TEST(branch_git_child_env_has_longpaths) {
    bi_git_env_t saved;
    bi_git_env_save(&saved, true);
    cbm_setenv("GIT_CONFIG_COUNT", "1", 1);
    cbm_setenv("GIT_CONFIG_KEY_0", "user.name", 1);
    cbm_setenv("GIT_CONFIG_VALUE_0", "parent", 1);
    wchar_t *block = cbm_git_child_env_block();
    int counts = 0;
    bool key = false;
    bool value = false;
    bool parent_item = false;
    for (const wchar_t *e = block; e && *e; e += wcslen(e) + 1) {
        counts += wcsncmp(e, L"GIT_CONFIG_COUNT=", 17) == 0;
        key = key || wcscmp(e, L"GIT_CONFIG_KEY_0=core.longpaths") == 0;
        value = value || wcscmp(e, L"GIT_CONFIG_VALUE_0=true") == 0;
        parent_item = parent_item || wcscmp(e, L"GIT_CONFIG_KEY_0=user.name") == 0 ||
                      wcscmp(e, L"GIT_CONFIG_VALUE_0=parent") == 0;
    }
    cbm_git_child_env_free(block);
    bi_git_env_restore(&saved);
    ASSERT_EQ(counts, 1);
    ASSERT_TRUE(key);
    ASSERT_TRUE(value);
    ASSERT_FALSE(parent_item);
    PASS();
}
#endif

TEST(branch_git_head_read_caps_line_length) {
    char *tmp = th_mktempdir("cbm_bi_longline");
    ASSERT_NOT_NULL(tmp);
    char repo[512];
    snprintf(repo, sizeof(repo), "%s", tmp);
    char p[900];
    snprintf(p, sizeof(p), "%s/.git/refs/heads", repo);
    ASSERT_TRUE(cbm_mkdir_p(p, 0755));
    static char big[10000];
    snprintf(big, sizeof(big), "ref: refs/heads/");
    size_t used = strlen(big);
    memset(big + used, 'x', 6000);
    big[used + 6000] = '\n';
    big[used + 6001] = '\0';
    snprintf(p, sizeof(p), "%s/.git/HEAD", repo);
    ASSERT_TRUE(bi_put(p, big));
    cbm_git_head_t head;
    ASSERT_NEQ(cbm_git_head_read(repo, &head), 0);
    cbm_git_head_free(&head);

    /* main only in packed-refs, after a 6000-byte line. */
    ASSERT_TRUE(bi_put(p, "ref: refs/heads/main\n"));
    static char packed[10000];
    memset(packed, 'y', 6000);
    snprintf(packed + 6000, sizeof(packed) - 6000,
             "\n0123456789abcdef0123456789abcdef01234567 refs/heads/main\n");
    snprintf(p, sizeof(p), "%s/.git/packed-refs", repo);
    ASSERT_TRUE(bi_put(p, packed));
    ASSERT_EQ(cbm_git_head_read(repo, &head), 0);
    ASSERT_STR_EQ(head.branch, "main");
    ASSERT_STR_EQ(head.head_sha, "0123456789abcdef0123456789abcdef01234567");
    cbm_git_head_free(&head);
    th_rmtree(repo);
    PASS();
}

/* main → feature → main → feature: each switch parks the outgoing graph, the
 * first visit to a branch seeds from the other, a revisit restores. */
TEST(branch_switch_parks_and_restores) {
    char *repo = bi_make_repo("cbm_bi_switch");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_EQ(bi_count("only_main"), 1);
    ASSERT_EQ(bi_count("only_feature"), 0);

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_STR_EQ(r.from, "main");
    ASSERT_EQ(r.reparsed, 1); /* b.py only: the seed already holds common.py */
    ASSERT_EQ(bi_count("only_main"), 0);
    ASSERT_EQ(bi_count("only_feature"), 1);
    ASSERT_TRUE(bi_is_parked("main"));

    /* Back to main: main's parked graph comes back. git rewrote a.py, but to
     * the bytes main's graph was built from, so nothing is re-extracted. */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(r.from, "feature");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_count("only_main"), 1);
    ASSERT_EQ(bi_count("only_feature"), 0);
    ASSERT_TRUE(bi_is_parked("feature"));
    ASSERT_FALSE(bi_is_parked("main"));

    /* The seeded run stamped "feature" on its graph (an incremental run keeps
     * the stored Branch node otherwise), so this restore finds it parked
     * under the right name. */
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_count("only_feature"), 1);
    ASSERT_EQ(bi_count("only_main"), 0);

    /* Same branch again: nothing to switch. */
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_EQ(r.reparsed, 0);

    bi_cleanup(repo);
    PASS();
}

/* An empty file only one branch has (a new package's __init__.py) is deleted
 * and recreated by each checkout, so it comes back under a new mtime. Its
 * stored row has no digest, and the restore used to re-extract it, running
 * the whole incremental pass on every switch back. Soak finding (django). */
TEST(branch_restore_keeps_recreated_empty_file) {
    char *repo = bi_make_repo("cbm_bi_empty");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    th_write_file(TH_PATH(repo, "src/pkg/__init__.py"), "");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m pkg"), 0);

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");

    /* Let the recreated file's mtime differ from the stored one even on a
     * file system with coarse timestamps. */
    cbm_usleep(1100000);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r.reparsed, 0);

    bi_cleanup(repo);
    PASS();
}

/* Every detached HEAD shares one "detached" slot. */
TEST(branch_switch_detached_shares_one_slot) {
    char *repo = bi_make_repo("cbm_bi_detached");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q --detach feature"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("only_feature"), 1);

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(r.from, "detached");
    ASSERT_TRUE(bi_is_parked("detached"));
    ASSERT_EQ(bi_count("only_main"), 1);

    bi_cleanup(repo);
    PASS();
}

/* Parked databases live under branches/<project>/ — list_projects must not
 * report them; index_status names the indexed branch and the parked ones;
 * delete_project removes them with the project. */
TEST(branch_parked_hidden_status_and_delete) {
    char *repo = bi_make_repo("cbm_bi_mcp");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_TRUE(bi_is_parked("main"));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_handle_tool(srv, "list_projects", "{}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, g_bi_project));
    ASSERT_NULL(strstr(resp, "branches"));
    ASSERT_NULL(strstr(resp, "\\\"main\\\""));
    free(resp);

    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", g_bi_project);
    resp = cbm_mcp_handle_tool(srv, "index_status", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"indexed_branch\\\":\\\"feature\\\""));
    ASSERT_NOT_NULL(strstr(resp, "\\\"index_matches_checkout\\\":true"));
    ASSERT_NOT_NULL(strstr(resp, "\\\"parked_branches\\\":[\\\"main\\\"]"));
    /* Slot sizes, their total and the limits. */
    ASSERT_NOT_NULL(strstr(resp, "\\\"parked_slots\\\":[{\\\"branch\\\":\\\"main\\\""));
    ASSERT_NOT_NULL(strstr(resp, "\\\"compressed\\\":true"));
    ASSERT_NOT_NULL(strstr(resp, "\\\"parked_bytes\\\":"));
    ASSERT_NOT_NULL(strstr(resp, "\\\"branch_index_keep\\\":8"));
    ASSERT_NOT_NULL(strstr(resp, "\\\"branch_index_budget_bytes\\\":2147483648"));
    free(resp);

    /* Checked out but not yet reindexed: the mismatch is visible. */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    resp = cbm_mcp_handle_tool(srv, "index_status", args);
    ASSERT_NOT_NULL(strstr(resp, "\\\"index_matches_checkout\\\":false"));
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "delete_project", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"parked_branches_deleted\\\":1"));
    free(resp);
    char **list = NULL;
    ASSERT_EQ(bi_parked(&list), 0);
    char dir[1024];
    char db[1024];
    ASSERT_TRUE(
        cbm_branch_slots_dir(bi_db_path(db, sizeof(db)), g_bi_project, NULL, dir, sizeof(dir)));
    ASSERT_FALSE(cbm_file_exists(dir));
    ASSERT_FALSE(cbm_file_exists(db));

    cbm_mcp_server_free(srv);
    bi_cleanup(repo);
    PASS();
}

/* keep = 2: visiting four branches keeps only the two most recently parked. */
TEST(branch_park_cap_evicts_oldest) {
    char *repo = bi_make_repo("cbm_bi_cap");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_git(repo, "branch -q b1"), 0);
    ASSERT_EQ(bi_git(repo, "branch -q b2"), 0);
    ASSERT_EQ(bi_git(repo, "branch -q b3"), 0);
    ASSERT_EQ(bi_index(repo, 2).rc, 0); /* main */
    const char *order[] = {"b1", "b2", "b3", "feature"};
    for (int i = 0; i < 4; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "checkout -q %s", order[i]);
        ASSERT_EQ(bi_git(repo, cmd), 0);
        /* Park times are whole seconds; keep each park in its own second so
         * "oldest" is unambiguous. */
        cbm_usleep(600000);
        cbm_usleep(600000);
        ASSERT_EQ(bi_index(repo, 2).rc, 0);
    }
    /* Parked in order: main, b1, b2, b3 → the two newest survive. */
    char **list = NULL;
    int n = bi_parked(&list);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(list[0], "b2");
    ASSERT_STR_EQ(list[1], "b3");
    cbm_branch_slots_free_list(list, n);

    bi_cleanup(repo);
    PASS();
}

/* Off switch (keep 0, and the branch_index config key through
 * index_repository): one database, reindexed in place, nothing parked. */
TEST(branch_index_disabled_reindexes_in_place) {
    char *repo = bi_make_repo("cbm_bi_off");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, 0).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, 0);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_EQ(bi_count("only_feature"), 1);
    ASSERT_EQ(bi_count("only_main"), 0);
    char **list = NULL;
    ASSERT_EQ(bi_parked(&list), 0);

    /* Same through the MCP tool with the config key off. */
    cbm_config_t *cfg = cbm_config_open(cbm_resolve_cache_dir());
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_BRANCH_INDEX, "false"), 0);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    char args[512];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", repo);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"switch\\\":\\\"none\\\""));
    free(resp);
    ASSERT_EQ(bi_parked(&list), 0);
    ASSERT_EQ(bi_count("only_main"), 1);
    ASSERT_EQ(bi_count("only_feature"), 0);

    /* Back on: the next switch parks again. */
    ASSERT_EQ(cbm_config_delete(cfg, CBM_CONFIG_BRANCH_INDEX), 0);
    cbm_config_close(cfg);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"switch\\\":\\\"seeded\\\""));
    free(resp);
    ASSERT_TRUE(bi_is_parked("main"));

    cbm_mcp_server_free(srv);
    bi_cleanup(repo);
    PASS();
}

/* Set (content != NULL) or delete the project's ADR in its current database. */
static int bi_adr_put(const char *content) {
    char db[1024];
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    if (!s) {
        return CBM_STORE_ERR;
    }
    int rc = content ? cbm_store_adr_store(s, g_bi_project, content)
                     : cbm_store_adr_delete(s, g_bi_project);
    cbm_store_close(s);
    return rc;
}

/* The project's ADR in its current database into buf; "" when none. */
static const char *bi_adr(char *buf, size_t n) {
    buf[0] = '\0';
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    if (!s) {
        return buf;
    }
    cbm_adr_t adr = {0};
    if (cbm_store_adr_get(s, g_bi_project, &adr) == CBM_STORE_OK) {
        snprintf(buf, n, "%s", adr.content ? adr.content : "");
        cbm_store_adr_free(&adr);
    }
    cbm_store_close(s);
    return buf;
}

/* The ADR is about the project, not a branch: whatever was written last
 * survives every switch, seeded or restored. */
TEST(branch_switch_keeps_newest_adr) {
    char *repo = bi_make_repo("cbm_bi_adr");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char adr[256];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_adr_put("A"), CBM_STORE_OK);

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "A");
    ASSERT_EQ(bi_adr_put("B"), CBM_STORE_OK);

    /* main's parked database still holds "A". */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "B");

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "B");

    /* A delete is carried too: the outgoing database is the authority, so
     * main's parked "B" must not come back. */
    ASSERT_EQ(bi_adr_put(NULL), CBM_STORE_OK);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "");

    bi_cleanup(repo);
    PASS();
}

/* The first run on a new branch is incremental, and it refreshes an existing
 * .code-cortex artifact: that artifact must already name the new branch, or a
 * checkout bootstrapped from it parks the graph under the old one. */
TEST(branch_seeded_artifact_names_new_branch) {
    char *repo = bi_make_repo("cbm_bi_art");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_exists(repo));

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(r.reparsed, 1); /* incremental, so the artifact took the FAST re-export */

    char *dir = th_mktempdir("cbm_bi_art_import");
    ASSERT_NOT_NULL(dir);
    char imported[1024];
    snprintf(imported, sizeof(imported), "%s/imported.db", dir);
    ASSERT_EQ(cbm_artifact_import(repo, imported), 0);
    cbm_store_t *s = cbm_store_open_path_query(imported);
    ASSERT_NOT_NULL(s);
    char qn[1024] = "";
    ASSERT_EQ(cbm_store_get_branch(s, g_bi_project, qn, sizeof(qn), NULL, 0), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_STR_EQ(cbm_branch_slug_from_qn(qn, g_bi_project), "feature");

    th_rmtree(dir);
    bi_cleanup(repo);
    PASS();
}

/* The in-process index run (the watcher's path when CBM_INDEX_SUPERVISOR=0)
 * reads the branch_index config key like index_repository does. */
TEST(branch_index_config_applies_in_process) {
    char *repo = bi_make_repo("cbm_bi_inproc");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_config_t *cfg = cbm_config_open(cbm_resolve_cache_dir());
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_BRANCH_INDEX, "false"), 0);
    char *project = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(project);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", project);
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", repo);
    free(project);

    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0);
    char **list = NULL;
    int parked = bi_parked(&list);
    cbm_branch_slots_free_list(list, parked);
    ASSERT_EQ(bi_count("only_feature"), 1);

    ASSERT_EQ(cbm_config_delete(cfg, CBM_CONFIG_BRANCH_INDEX), 0);
    cbm_config_close(cfg);
    ASSERT_EQ(parked, 0);

    bi_cleanup(repo);
    PASS();
}

/* index_status's index_matches_checkout for the project: 1, 0, or -1 when
 * the field is missing. */
static int bi_status_matches(cbm_mcp_server_t *srv) {
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", g_bi_project);
    char *resp = cbm_mcp_handle_tool(srv, "index_status", args);
    int r = !resp                                                  ? -1
            : strstr(resp, "\\\"index_matches_checkout\\\":true")  ? 1
            : strstr(resp, "\\\"index_matches_checkout\\\":false") ? 0
                                                                   : -1;
    free(resp);
    return r;
}

/* What session start compares: the checkout's HEAD files against the
 * identity recorded in the current database. */
static const char *nz_s(const char *s) {
    return s ? s : "(null)";
}

static bool bi_head_matches_index(const char *repo) {
    cbm_branch_id_t current;
    cbm_branch_id_t indexed = {};
    if (!cbm_branch_id_from_head(repo, &current)) {
        cbm_branch_id_free(&current);
        return true;
    }
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    bool known = s && cbm_branch_id_indexed(s, g_bi_project, &indexed);
    cbm_store_close(s);
    bool same = !known || cbm_branch_id_same(&indexed, &current);
    if (!same) {
        fprintf(stderr, "    head/index mismatch: index branch=%.60s... sha=%s detached=%d | "
                        "head branch=%.60s... sha=%s detached=%d\n",
                nz_s(indexed.branch), indexed.head_sha, indexed.is_detached,
                nz_s(current.branch), current.head_sha, current.is_detached);
    }
    cbm_branch_id_free(&indexed);
    cbm_branch_id_free(&current);
    return same;
}

/* Identity is the branch name plus HEAD commit, not the slug: a detached HEAD
 * that moved, a named branch that gained a commit, and two branches whose
 * names slug alike are all "another checkout" -- for index_status and for the
 * session-start check -- until the next index run records the new HEAD. */
TEST(branch_identity_is_name_and_head) {
    char *repo = bi_make_repo("cbm_bi_ident");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Detached at main's commit, then at feature's: both slug "detached". */
    ASSERT_EQ(bi_git(repo, "checkout -q --detach main"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_status_matches(srv), 1);
    ASSERT_TRUE(bi_head_matches_index(repo));
    ASSERT_EQ(bi_git(repo, "checkout -q --detach feature"), 0);
    ASSERT_EQ(bi_status_matches(srv), 0);
    ASSERT_FALSE(bi_head_matches_index(repo));
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none"); /* same slot: an incremental pass, no park */
    ASSERT_EQ(bi_status_matches(srv), 1);
    ASSERT_TRUE(bi_head_matches_index(repo));

    /* A named branch that gains a commit while nothing watched. The HEAD
     * commit is read from packed-refs once the loose ref is packed. */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_status_matches(srv), 1);
    th_write_file(TH_PATH(repo, "src/common.py"), "def common_helper():\n    return 3\n");
    ASSERT_EQ(bi_git(repo, "commit -q -am bump"), 0);
    ASSERT_EQ(bi_git(repo, "pack-refs --all"), 0);
    ASSERT_EQ(bi_status_matches(srv), 0);
    ASSERT_FALSE(bi_head_matches_index(repo));
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 1); /* incremental: only the changed file */
    ASSERT_EQ(bi_status_matches(srv), 1);
    ASSERT_TRUE(bi_head_matches_index(repo));

    /* "a/b" and "a-b": distinct identities (and slugs, and slots). */
    ASSERT_EQ(bi_git(repo, "branch -q a/b main"), 0);
    ASSERT_EQ(bi_git(repo, "branch -q a-b main"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q a/b"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_status_matches(srv), 1);
    ASSERT_EQ(bi_git(repo, "checkout -q a-b"), 0);
    ASSERT_EQ(bi_status_matches(srv), 0);
    ASSERT_FALSE(bi_head_matches_index(repo));
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_status_matches(srv), 1);

    cbm_mcp_server_free(srv);
    bi_cleanup(repo);
    PASS();
}

/* The file-only HEAD read agrees with git on the commit, through a loose ref,
 * packed-refs, a detached HEAD, a linked worktree and a submodule. */
TEST(branch_head_read_matches_git) {
    char *repo = bi_make_repo("cbm_bi_headsha");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char wt[512];
    snprintf(wt, sizeof(wt), "%s/wt", repo);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "worktree add -q -b side \"%s\" feature", wt);
    ASSERT_EQ(bi_git(repo, cmd), 0);

    /* A submodule: its .git is a gitlink into the superproject's
     * .git/modules, with no commondir. */
    char sub_cmd[1200];
    snprintf(sub_cmd, sizeof(sub_cmd), "-c protocol.file.allow=always submodule add -q \"%s\" mod",
             repo);
    ASSERT_EQ(bi_git(repo, sub_cmd), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m submodule"), 0);
    char mod[600];
    snprintf(mod, sizeof(mod), "%s/mod/src", repo); /* below the submodule root */

    for (int round = 0; round < 3; round++) {
        if (round == 1) {
            ASSERT_EQ(bi_git(repo, "pack-refs --all"), 0);
        } else if (round == 2) {
            ASSERT_EQ(bi_git(repo, "checkout -q --detach feature"), 0);
        }
        const char *roots[] = {repo, wt, mod};
        for (int i = 0; i < 3; i++) {
            cbm_git_context_t ctx = {0};
            ASSERT_EQ(cbm_git_context_resolve(roots[i], &ctx), 0);
            cbm_git_head_t head;
            ASSERT_EQ(cbm_git_head_read(roots[i], &head), 0);
            ASSERT_STR_EQ(head.head_sha, ctx.head_sha);
            ASSERT_EQ(head.is_detached, ctx.is_detached);
            if (!ctx.is_detached) {
                ASSERT_STR_EQ(head.branch, ctx.branch);
            }
            cbm_git_head_free(&head);
            cbm_git_context_free(&ctx);
        }
    }
    th_rmtree(repo);
    PASS();
}

/* A branch whose name contains the QN marker is still one branch: the second
 * run on it is not a switch, nothing is parked under a truncated name, and
 * the index matches the checkout. */
TEST(branch_name_containing_marker) {
    char *repo = bi_make_repo("cbm_bi_marker");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q -b foo.__branch__.bar"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_STR_EQ(r.from, "foo.__branch__.bar");
    ASSERT_FALSE(bi_is_parked("bar"));
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(bi_status_matches(srv), 1);
    cbm_mcp_server_free(srv);

    bi_cleanup(repo);
    PASS();
}

/* Databases written before the identity was kept current: one whose Branch
 * properties carry a stale HEAD (an older build's incremental runs never
 * refreshed them) and one with no properties at all. Neither may cause a
 * branch switch or a full rebuild; the stale one costs one incremental pass,
 * after which the identity is current. */
TEST(branch_identity_from_older_databases) {
    char *repo = bi_make_repo("cbm_bi_legacy");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char db[1024];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    char qn[1024];
    char props[4096];
    ASSERT_EQ(
        cbm_store_get_branch_props(s, g_bi_project, qn, sizeof(qn), NULL, 0, props, sizeof(props)),
        CBM_STORE_OK);
    char *sha = strstr(props, "\"head_sha\":\"");
    ASSERT_NOT_NULL(sha);
    sha += strlen("\"head_sha\":\"");
    memset(sha, '0', 40); /* a commit that is not HEAD */
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "main", props), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_FALSE(bi_head_matches_index(repo));
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_TRUE(bi_head_matches_index(repo));

    s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "main", "{}"), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_TRUE(bi_head_matches_index(repo)); /* slug only: same branch */
    /* Built while git refused the checkout (is_git false, slot "working-tree"):
     * nothing to compare, so no session is marked pending for it. */
    char nongit_qn[1100];
    snprintf(nongit_qn, sizeof(nongit_qn), "%s.__branch__.working-tree", g_bi_project);
    s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, nongit_qn, "working-tree",
                                   "{\"is_git\":false,\"branch\":\"\",\"head_sha\":\"\"}"),
              CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_TRUE(bi_head_matches_index(repo));
    s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "main", "{}"), CBM_STORE_OK);
    cbm_store_close(s);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_STR_EQ(r.from, "main");
    ASSERT_EQ(r.reparsed, 0);

    bi_cleanup(repo);
    PASS();
}

/* A project indexed at a directory BELOW the checkout root: an offline branch
 * change must still be seen by the session-start check (which reads HEAD
 * from files) and by index_status, and the next run parks and seeds as for a
 * root project. */
TEST(branch_subdir_root_sees_offline_switch) {
    char *repo = bi_make_repo("cbm_bi_subdir");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/src", repo);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    ASSERT_EQ(bi_index(sub, -1).rc, 0);
    ASSERT_TRUE(bi_head_matches_index(sub));
    ASSERT_EQ(bi_status_matches(srv), 1);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_FALSE(bi_head_matches_index(sub));
    ASSERT_EQ(bi_status_matches(srv), 0);

    bi_run_t r = bi_index(sub, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_STR_EQ(r.from, "main");
    ASSERT_TRUE(bi_is_parked("main"));
    ASSERT_TRUE(bi_head_matches_index(sub));
    ASSERT_EQ(bi_status_matches(srv), 1);

    cbm_mcp_server_free(srv);
    char db[1024];
    bi_db_path(db, sizeof(db));
    (void)cbm_branch_slots_delete_all(db, g_bi_project);
    cbm_unlink(db);
    cbm_remove_db_sidecars(db);
    th_rmtree(repo);
    PASS();
}

/* <project>.db gone (an interrupted full rebuild, a manual delete) while the
 * checked-out branch has a parked database: that one is restored, and the
 * incremental pass takes it from there. */
TEST(branch_missing_live_db_restores_parked) {
    char *repo = bi_make_repo("cbm_bi_nolive");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked */
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_EQ(cbm_unlink(db), 0);
    cbm_remove_db_sidecars(db);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(r.from, "");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_count("only_main"), 1);
    ASSERT_FALSE(bi_is_parked("main"));

    bi_cleanup(repo);
    PASS();
}

/* The bulk index reset also clears parked databases whose <project>.db is
 * already gone, but keeps those of a project another process is indexing. */
TEST(branch_reset_clears_orphaned_slots) {
    char *cache = th_mktempdir("cbm_bi_reset");
    ASSERT_NOT_NULL(cache);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", cache);
    cache = saved;
    char old_cache[1024] = "";
    bool had_cache = cbm_safe_getenv("CBM_CACHE_DIR", old_cache, sizeof(old_cache), NULL) != NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *files[] = {"live.db",
                           "branches/live/main.db",
                           "branches/orphan/main.db",
                           "branches/orphan/feature.db",
                           "branches/busy/main.db",
                           "branches/orphan/k1/zst.db.zst",
                           "branches/orphan/k1/zst.db.unz"};
    for (int i = 0; i < 7; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", cache, files[i]);
        ASSERT_EQ(th_write_file(path, "x"), 0);
    }
    char busy[1024];
    snprintf(busy, sizeof(busy), "%s/busy.db", cache);
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(busy, &lease), 0);

    /* live.db, its parked main.db, and orphan's three parked databases (one
     * compressed, under a root key, beside a scratch file). */
    ASSERT_EQ(cbm_remove_indexes(NULL), 5);
    char path[1024];
    snprintf(path, sizeof(path), "%s/branches/live", cache);
    ASSERT_FALSE(cbm_file_exists(path));
    snprintf(path, sizeof(path), "%s/branches/orphan", cache);
    ASSERT_FALSE(cbm_file_exists(path));
    snprintf(path, sizeof(path), "%s/branches/busy/main.db", cache);
    ASSERT_TRUE(cbm_file_exists(path));

    cbm_db_lease_release(lease);
    ASSERT_EQ(cbm_remove_indexes(NULL), 1); /* busy's parked main.db */
    ASSERT_FALSE(cbm_file_exists(path));
    snprintf(path, sizeof(path), "%s/branches", cache);
    ASSERT_FALSE(cbm_file_exists(path));

    if (had_cache) {
        cbm_setenv("CBM_CACHE_DIR", old_cache, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_rmtree(cache);
    PASS();
}

/* A restore replaces the CONTENT of <project>.db, not the file: a reader
 * that already has it open (a tool worker's cached store) sees the restored
 * graph on its next read, instead of the outgoing one forever from an
 * unlinked inode. */
TEST(branch_restore_is_visible_to_open_readers) {
    char *repo = bi_make_repo("cbm_bi_reader");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked, feature live */

    char db[1024];
    cbm_store_t *reader = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(reader);
    cbm_node_t *nodes = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_name(reader, g_bi_project, "only_feature", &nodes, &count),
              CBM_STORE_OK);
    cbm_store_free_nodes(nodes, count);
    ASSERT_EQ(count, 1);

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");

    cbm_store_end_reads(reader);
    nodes = NULL;
    count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_name(reader, g_bi_project, "only_main", &nodes, &count),
              CBM_STORE_OK);
    cbm_store_free_nodes(nodes, count);
    ASSERT_EQ(count, 1);
    cbm_store_close(reader);

    bi_cleanup(repo);
    PASS();
}

/* The checkout changes between creating the pipeline and running it (the
 * run waited on the index lease or the pipeline lock): the run must index,
 * park and stamp the checkout it actually reads. */
TEST(branch_switch_uses_checkout_at_run_time) {
    char *repo = bi_make_repo("cbm_bi_late");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main */
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *action = NULL;
    const char *from = NULL;
    const char *to = NULL;
    cbm_pipeline_get_branch_switch(p, &action, &from, &to, NULL);
    ASSERT_STR_EQ(action, "seeded");
    ASSERT_STR_EQ(from, "main");
    ASSERT_STR_EQ(to, "feature");
    cbm_pipeline_free(p);
    ASSERT_TRUE(bi_is_parked("main"));
    ASSERT_EQ(bi_count("only_feature"), 1);
    ASSERT_TRUE(bi_head_matches_index(repo));

    bi_cleanup(repo);
    PASS();
}

#ifndef _WIN32
/* Pin path's mtime to a fixed instant, to the nanosecond. */
static int bi_pin_mtime(const char *path) {
    struct timespec ts[2] = {{1700000000, 123456789}, {1700000000, 123456789}};
    return utimensat(AT_FDCWD, path, ts, 0);
}

/* A file whose bytes differ between two checkouts while its size and mtime
 * do not: after a branch switch, and after a same-branch HEAD move, the run
 * must notice and re-extract it rather than trust the stored hash. */
TEST(branch_switch_rehashes_same_size_same_mtime) {
    char *repo = th_mktempdir("cbm_bi_mtime");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char x[512];
    snprintf(x, sizeof(x), "%s/src/x.py", repo);
    ASSERT_EQ(bi_git(repo, "init -q -b main"), 0);
    th_write_file(x, "def aaa():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m main"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q -b feature"), 0);
    th_write_file(x, "def bbb():\n    return 1\n"); /* same size */
    ASSERT_EQ(bi_git(repo, "commit -q -am feature"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);

    ASSERT_EQ(bi_pin_mtime(x), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_count("aaa"), 1);

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_pin_mtime(x), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(r.reparsed, 1);
    ASSERT_EQ(bi_count("bbb"), 1);
    ASSERT_EQ(bi_count("aaa"), 0);

    /* Same branch, HEAD moved (a pull): same size, same mtime again. */
    th_write_file(x, "def ccc():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "commit -q -am pull"), 0);
    ASSERT_EQ(bi_pin_mtime(x), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_EQ(r.reparsed, 1);
    ASSERT_EQ(bi_count("ccc"), 1);
    ASSERT_EQ(bi_count("bbb"), 0);

    /* Identity unchanged: the size+mtime fast path holds (nothing re-read). */
    r = bi_index(repo, -1);
    ASSERT_EQ(r.reparsed, 0);

    /* A legacy Branch node with no properties records no commit: the graph's
     * age is unknown, so the next run after a pull verifies too. */
    char db[1024];
    char qn[1024];
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_get_branch(s, g_bi_project, qn, sizeof(qn), NULL, 0), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "feature", "{}"), CBM_STORE_OK);
    cbm_store_close(s);
    th_write_file(x, "def ddd():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "commit -q -am pull2"), 0);
    ASSERT_EQ(bi_pin_mtime(x), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 1);
    ASSERT_EQ(bi_count("ddd"), 1);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.reparsed, 0); /* stamped with a commit now: fast path again */

    bi_cleanup(repo);
    PASS();
}
#endif

/* Rows of nodes_fts matching term (a full-text query), -1 on error. */
static int bi_fts_hits(const char *term) {
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    sqlite3 *h = cbm_store_get_db(s);
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (h && sqlite3_prepare_v2(h, "SELECT count(*) FROM nodes_fts WHERE nodes_fts MATCH ?1", -1,
                                &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, term, -1, SQLITE_TRANSIENT);
        n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    }
    sqlite3_finalize(st);
    cbm_store_close(s);
    return n;
}

/* The Branch node's full-text entry follows the stamp: on a switch whose
 * incremental pass changes nothing (the stamp is the only write) and on one
 * that re-dumps, free-text search finds the new branch and not the old. */
TEST(branch_stamp_keeps_fts_in_step) {
    char *repo = th_mktempdir("cbm_bi_fts");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_git(repo, "init -q -b alphaone"), 0);
    th_write_file(TH_PATH(repo, "src/x.py"), "def aaa():\n    return 1\n");
    /* Enough files that adding one stays an incremental pass. */
    th_write_file(TH_PATH(repo, "src/z1.py"), "def zz1():\n    return 1\n");
    th_write_file(TH_PATH(repo, "src/z2.py"), "def zz2():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m one"), 0);
    ASSERT_EQ(bi_git(repo, "branch -q betatwo"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q -b gammathree"), 0);
    th_write_file(TH_PATH(repo, "src/y.py"), "def yyy():\n    return 2\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m three"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q alphaone"), 0);

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_fts_hits("alphaone"), 1);

    ASSERT_EQ(bi_git(repo, "checkout -q betatwo"), 0); /* same commit */
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_fts_hits("betatwo"), 1);
    ASSERT_EQ(bi_fts_hits("alphaone"), 0);

    ASSERT_EQ(bi_git(repo, "checkout -q gammathree"), 0); /* re-dumps */
    r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(r.reparsed, 1);
    ASSERT_EQ(bi_fts_hits("gammathree"), 1);
    ASSERT_EQ(bi_fts_hits("betatwo"), 0);

    bi_cleanup(repo);
    PASS();
}

/* A Branch node whose HAS_BRANCH edge was left behind (a stamp torn before
 * stamps were one transaction) is repaired by the next run, even though the
 * node itself already names the checkout. */
TEST(branch_stamp_repairs_torn_edge) {
    char *repo = bi_make_repo("cbm_bi_torn");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);

    char db[1024];
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    char sql[1400];
    snprintf(sql, sizeof(sql),
             "UPDATE edges SET properties = '{\"branch\":\"stale\"}' WHERE project = '%s' AND "
             "type = 'HAS_BRANCH';",
             g_bi_project);
    ASSERT_EQ(cbm_store_exec(s, sql), CBM_STORE_OK);
    cbm_store_close(s);

    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 0);
    s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    sqlite3_stmt *st = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(cbm_store_get_db(s),
                                 "SELECT count(*) FROM edges e JOIN nodes n ON n.id = e.target_id "
                                 "WHERE e.type = 'HAS_BRANCH' AND e.properties = n.properties",
                                 -1, &st, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int in_step = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    cbm_store_close(s);
    ASSERT_EQ(in_step, 1);

    bi_cleanup(repo);
    PASS();
}

/* HAS_BRANCH edges from the Project node to the Branch node carrying the
 * node's properties, -1 on error. */
static int bi_branch_edges_in_step(void) {
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    sqlite3 *h = cbm_store_get_db(s);
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (h && sqlite3_prepare_v2(h,
                                "SELECT count(*) FROM edges e JOIN nodes b ON b.id = e.target_id "
                                "JOIN nodes pn ON pn.id = e.source_id WHERE e.project = ?1 AND "
                                "e.type = 'HAS_BRANCH' AND b.label = 'Branch' AND "
                                "pn.label = 'Project' AND e.properties = b.properties",
                                -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, g_bi_project, -1, SQLITE_TRANSIENT);
        n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    }
    sqlite3_finalize(st);
    cbm_store_close(s);
    return n;
}

/* A Branch node that lost its HAS_BRANCH edge (a legacy or torn database)
 * is not current although the node names the checkout: the next run puts
 * the edge back, and the stamp itself recreates it. */
TEST(branch_stamp_recreates_missing_edge) {
    char *repo = bi_make_repo("cbm_bi_noedge");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_branch_edges_in_step(), 1);

    char db[1024];
    char sql[1400];
    snprintf(sql, sizeof(sql), "DELETE FROM edges WHERE project = '%s' AND type = 'HAS_BRANCH';",
             g_bi_project);
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_exec(s, sql), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_EQ(bi_branch_edges_in_step(), 0);

    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_branch_edges_in_step(), 1);

    /* set itself: rewriting the node in place also restores the edge. */
    s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_exec(s, sql), CBM_STORE_OK);
    char qn[1024];
    ASSERT_EQ(cbm_store_get_branch(s, g_bi_project, qn, sizeof(qn), NULL, 0), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "main", "{}"), CBM_STORE_OK);
    ASSERT_TRUE(cbm_store_branch_is(s, g_bi_project, qn, "main", "{}"));
    cbm_store_close(s);
    ASSERT_EQ(bi_branch_edges_in_step(), 1);

    bi_cleanup(repo);
    PASS();
}

/* An outgoing database whose ADR cannot be read may hold the only copy of
 * the newest ADR: the switch must keep it in place as the seed rather than
 * restore a parked database over it. */
TEST(branch_unreadable_adr_blocks_restore) {
    char *repo = bi_make_repo("cbm_bi_adrerr");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char adr[256];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_adr_put("OLD"), CBM_STORE_OK);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked with "OLD" */
    ASSERT_TRUE(bi_is_parked("main"));

    /* The live ADR becomes unreadable: a view of the wrong shape stands in
     * for its table, which a read-write open (CREATE TABLE IF NOT EXISTS)
     * leaves in place. */
    char db[1024];
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_exec(s, "DROP TABLE project_summaries;"
                                "CREATE VIEW project_summaries AS SELECT 1 AS x;"),
              CBM_STORE_OK);
    cbm_store_close(s);

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_STR_NEQ(bi_adr(adr, sizeof(adr)), "OLD");
    ASSERT_EQ(bi_count("only_main"), 1);

    bi_cleanup(repo);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* A graph large enough that the session brief is carried over instead of
 * recomputed (bound lowered by the seam): after a switch the carried brief
 * would describe the other branch, so it is recomputed. */
TEST(branch_switch_recomputes_session_brief) {
    char *repo = bi_make_repo("cbm_bi_brief");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    /* feature also gains Go sources main does not have. */
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    th_write_file(TH_PATH(repo, "src/g.go"), "package src\n\nfunc GoOnly() int { return 1 }\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m go"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    cbm_pipeline_test_set_brief_recompute_max(1);

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, -1);
    cbm_pipeline_test_set_brief_recompute_max(0);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("GoOnly"), 1);

    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    char *brief = NULL;
    ASSERT_EQ(cbm_store_session_brief_get(s, g_bi_project, &brief), CBM_STORE_OK);
    cbm_store_close(s);
    bool has_go = brief && strstr(brief, "\"language\":\"Go\"") != NULL;
    free(brief);
    ASSERT_TRUE(has_go);

    bi_cleanup(repo);
    PASS();
}
#endif

#ifdef CBM_ENABLE_TEST_SEAMS
/* manage_adr in another process, committing just as a restore starts. */
static void bi_write_adr_new(void *data) {
    (void)data;
    (void)bi_adr_put("NEW");
}

/* An ADR written while a switch is under way, before the restore takes the
 * live database's write lock, survives the restore. */
TEST(branch_restore_keeps_concurrent_adr_write) {
    char *repo = bi_make_repo("cbm_bi_adrrace");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char adr[256];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_adr_put("A"), CBM_STORE_OK);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked with "A" */

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    cbm_branch_slots_test_before_restore(bi_write_adr_new, NULL);
    bi_run_t r = bi_index(repo, -1);
    cbm_branch_slots_test_before_restore(NULL, NULL);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "NEW");

    bi_cleanup(repo);
    PASS();
}
#endif

/* mtime (ns) of the repo's exported graph artifact; -1 when absent. */
static int64_t bi_artifact_mtime(const char *repo) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.code-cortex/graph.db.zst", repo);
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
#ifdef __APPLE__
    return ((int64_t)st.st_mtimespec.tv_sec * 1000000000LL) + st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st.st_mtime * 1000000000LL;
#else
    return ((int64_t)st.st_mtim.tv_sec * 1000000000LL) + st.st_mtim.tv_nsec;
#endif
}

/* The graph the repo's artifact holds: its Branch slug into slug and the
 * count of nodes named `symbol`; -1 when it cannot be imported. */
static int bi_artifact_probe(const char *repo, const char *symbol, char *slug, size_t slug_sz) {
    slug[0] = '\0';
    char *dir = th_mktempdir("cbm_bi_art_probe");
    if (!dir) {
        return -1;
    }
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", dir);
    char imported[1024];
    snprintf(imported, sizeof(imported), "%s/imported.db", saved);
    int count = -1;
    if (cbm_artifact_import(repo, imported) == 0) {
        cbm_store_t *s = cbm_store_open_path_query(imported);
        char qn[4096] = "";
        if (s && cbm_store_get_branch(s, g_bi_project, qn, sizeof(qn), NULL, 0) == CBM_STORE_OK) {
            const char *sl = cbm_branch_slug_from_qn(qn, g_bi_project);
            snprintf(slug, slug_sz, "%s", sl ? sl : "");
        }
        cbm_node_t *nodes = NULL;
        count = 0;
        if (s) {
            cbm_store_find_nodes_by_name(s, g_bi_project, symbol, &nodes, &count);
            cbm_store_free_nodes(nodes, count);
        }
        cbm_store_close(s);
    }
    th_rmtree(saved);
    return count;
}

/* A restore whose tree needs nothing re-extracted exports nothing through
 * the incremental path; the artifact must still follow the live database to
 * the restored branch. */
TEST(branch_restore_reexports_artifact) {
    char *repo = bi_make_repo("cbm_bi_artrest");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0); /* main, exported */
    cbm_pipeline_free(p);

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "seeded"); /* re-extracts, exports feature */
    char slug[256];
    ASSERT_EQ(bi_artifact_probe(repo, "only_feature", slug, sizeof(slug)), 1);
    ASSERT_STR_EQ(slug, "feature");

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r.reparsed, 0); /* nothing for the incremental pass to export */
    ASSERT_EQ(bi_artifact_probe(repo, "only_main", slug, sizeof(slug)), 1);
    ASSERT_STR_EQ(slug, "main");
    ASSERT_EQ(bi_artifact_probe(repo, "only_feature", slug, sizeof(slug)), 0);

    bi_cleanup(repo);
    PASS();
}

/* A switch to a branch with the same indexed contents writes only the stamp;
 * an existing artifact must still be re-exported under the new branch. */
TEST(branch_stamp_only_switch_reexports_artifact) {
    char *repo = bi_make_repo("cbm_bi_artstamp");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_exists(repo));

    ASSERT_EQ(bi_git(repo, "branch -q twin"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q twin"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(r.reparsed, 0); /* nothing but the stamp changed */

    char *dir = th_mktempdir("cbm_bi_artstamp_import");
    ASSERT_NOT_NULL(dir);
    char imported[1024];
    snprintf(imported, sizeof(imported), "%s/imported.db", dir);
    ASSERT_EQ(cbm_artifact_import(repo, imported), 0);
    cbm_store_t *s = cbm_store_open_path_query(imported);
    ASSERT_NOT_NULL(s);
    char qn[1024] = "";
    ASSERT_EQ(cbm_store_get_branch(s, g_bi_project, qn, sizeof(qn), NULL, 0), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_STR_EQ(cbm_branch_slug_from_qn(qn, g_bi_project), "twin");

    /* The next run on twin changes nothing -- the artifact directory is not
     * indexed -- so it does not export again: no loop. */
    int64_t before = bi_artifact_mtime(repo);
    cbm_usleep(1100000);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_artifact_mtime(repo), before);

    th_rmtree(dir);
    bi_cleanup(repo);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* A run that exports the artifact itself does not read the payload it just
 * wrote back from disk to check it is current; a later run, which did not
 * export, does check. */
TEST(artifact_export_run_skips_rehash) {
    char *repo = bi_make_repo("cbm_bi_artrehash");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    int before = cbm_artifact_test_payload_file_hashes();
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    int after_export = cbm_artifact_test_payload_file_hashes();
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_exists(repo));
    ASSERT_EQ(after_export, before);

    int64_t mtime = bi_artifact_mtime(repo);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_GT(cbm_artifact_test_payload_file_hashes(), after_export);
    ASSERT_EQ(bi_artifact_mtime(repo), mtime); /* current: not exported again */

    bi_cleanup(repo);
    PASS();
}
#endif

/* Whole file into buf (NUL-terminated, truncated to n-1); false when absent. */
static bool bi_read_text(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
    fclose(f);
    return true;
}
/* The dump's time is also the graph's generation (artifact freshness): two
 * dumps in quick succession must not share it, or a re-dump of a different
 * graph within one second would leave a stale artifact reading as current.
 * The stamp carries a sub-second fraction, and readers keep parsing it. */
static std::string bi_indexed_at(void) {
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    cbm_project_t proj = {0};
    std::string at;
    if (s && cbm_store_get_project(s, g_bi_project, &proj) == CBM_STORE_OK && proj.indexed_at) {
        at = proj.indexed_at;
    }
    cbm_project_free_fields(&proj);
    cbm_store_close(s);
    return at;
}

TEST(branch_dump_generation_is_sub_second) {
    char *repo = bi_make_repo("cbm_bi_gen");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    std::string first = bi_indexed_at();
    th_write_file(TH_PATH(repo, "src/gen.py"), "def gen_two():\n    return 2\n");
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* a change: a new dump, well within a second */
    std::string second = bi_indexed_at();
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    ASSERT_TRUE(first.find('.') != std::string::npos); /* "...T12:34:56.123456Z" */
    ASSERT_TRUE(first != second);
    bi_cleanup(repo);
    PASS();
}

/* The team artifact is committed. Committing it moves HEAD, which the Branch
 * node records; the artifact's freshness must not depend on it, or the next
 * run re-exports and leaves the tree dirty after every commit. artifact.json
 * records neither the HEAD commit's place in the Branch properties nor this
 * checkout's paths, so another clone's run does not see it as behind. */
TEST(branch_committed_artifact_stays_current) {
    char *repo = bi_make_repo("cbm_bi_artcommit");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_exists(repo));

    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/.code-cortex/artifact.json", repo);
    char text[16384];
    ASSERT_TRUE(bi_read_text(meta, text, sizeof(text)));
    ASSERT_NULL(strstr(text, "head_sha"));
    ASSERT_NULL(strstr(text, "canonical_root"));
    ASSERT_NULL(strstr(text, "worktree_root"));
    ASSERT_NULL(strstr(text, "git_common_dir"));

    ASSERT_EQ(bi_git(repo, "add -A"), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m artifact"), 0);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    int64_t before = bi_artifact_mtime(repo);
    cbm_usleep(1100000);
    bi_run_t r = bi_index(repo, -1); /* stamps the new HEAD commit */
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_EQ(bi_artifact_mtime(repo), before);
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    /* The tree git sees is clean: no re-exported artifact, nothing new. */
    char status[1024];
    snprintf(status, sizeof(status), "%s_status.txt", repo);
    char cmd[3072];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" status --porcelain > \"%s\" 2>%s", repo, status,
             bi_null_dev());
    ASSERT_EQ(system(cmd), 0);
    char out[4096];
    ASSERT_TRUE(bi_read_text(status, out, sizeof(out)));
    cbm_unlink(status);
    ASSERT_STR_EQ(out, "");

    bi_cleanup(repo);
    PASS();
}

/* An ADR write changes neither the files, the branch nor the dump time:
 * the artifact's generation carries an ADR digest, so a manage_adr update
 * makes a no-change persistent index export again with the new ADR. */
TEST(branch_artifact_stale_after_adr_update) {
    char *repo = bi_make_repo("cbm_bi_artadr");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    cbm_store_t *s = cbm_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_adr_store(s, g_bi_project, "## Decision\nuse slots\n"), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_FALSE(cbm_artifact_is_current(db, repo, g_bi_project));

    p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    ASSERT_EQ(cbm_pipeline_run(p), 0); /* no source change: exports for the ADR */
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    bi_cleanup(repo);
    PASS();
}

/* An export interrupted after publishing the payload but before the
 * metadata leaves a payload the metadata does not describe: not current. */
TEST(branch_artifact_payload_mismatch_is_not_current) {
    char *repo = bi_make_repo("cbm_bi_artpfp");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));
    /* Another generation's payload in place, the metadata unchanged. */
    char zst[1024];
    snprintf(zst, sizeof(zst), "%s/.code-cortex/graph.db.zst", repo);
    FILE *f = fopen(zst, "ab");
    ASSERT_NOT_NULL(f);
    fputs("another generation", f);
    fclose(f);
    ASSERT_FALSE(cbm_artifact_is_current(db, repo, g_bi_project));
    bi_cleanup(repo);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
static void bi_write_adr_now(void *ud) {
    cbm_store_t *s = cbm_store_open_path((const char *)ud);
    if (s) {
        (void)cbm_store_adr_store(s, g_bi_project, "## Decision\nwritten mid-export\n");
        cbm_store_close(s);
    }
}
#endif

/* An ADR committed after the export's snapshot and before its metadata read
 * leaves a payload with the old ADR: the metadata must not claim the new
 * ADR's generation, or the stale payload would read as current for good. */
TEST(branch_artifact_adr_during_export_is_not_current) {
#ifdef CBM_ENABLE_TEST_SEAMS
    char *repo = bi_make_repo("cbm_bi_artrace");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    char db[1024];
    bi_db_path(db, sizeof(db));
    cbm_artifact_test_after_payload(bi_write_adr_now, db);
    int rc = cbm_artifact_export(db, repo, g_bi_project, CBM_ARTIFACT_FAST);
    cbm_artifact_test_after_payload(NULL, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(cbm_artifact_is_current(db, repo, g_bi_project));
    bi_cleanup(repo);
#endif
    PASS();
}

extern "C" int cbm_install_handle_existing_indexes(const char *home, bool reset, bool dry_run);
extern "C" void cbm_set_auto_answer_for_test(int value);

/* The install/uninstall/update index prompts count parked databases too, so
 * a cache holding only orphaned ones (<project>.db already gone) is still
 * offered for deletion and cleared. Driven through install's gate. */
TEST(branch_index_prompt_counts_parked_only_cache) {
    char *cache = th_mktempdir("cbm_bi_gate");
    ASSERT_NOT_NULL(cache);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", cache);
    cache = saved;
    char old_cache[1024] = "";
    bool had_cache = cbm_safe_getenv("CBM_CACHE_DIR", old_cache, sizeof(old_cache), NULL) != NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    char slot[1024];
    snprintf(slot, sizeof(slot), "%s/branches/gone/main.db", cache);
    ASSERT_EQ(th_write_file(slot, "x"), 0);
    char zslot[1024];
    snprintf(zslot, sizeof(zslot), "%s/branches/gone/0123456789ab/feature.db.zst", cache);
    ASSERT_EQ(th_write_file(zslot, "x"), 0);

    cbm_set_auto_answer_for_test(1);
    int rc = cbm_install_handle_existing_indexes(NULL, true, false);
    cbm_set_auto_answer_for_test(0);
    ASSERT_EQ(rc, 1);
    ASSERT_FALSE(cbm_file_exists(slot));
    ASSERT_FALSE(cbm_file_exists(zslot));

    if (had_cache) {
        cbm_setenv("CBM_CACHE_DIR", old_cache, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_rmtree(cache);
    PASS();
}

/* A branch name of n bytes in components of at most 200 (each one a path
 * component in the loose ref store). */
static void bi_long_name(char *buf, size_t n, char fill) {
    size_t i = 0;
    while (i + 1 < n) {
        buf[i] = (i % 201 == 200) ? '/' : fill;
        i++;
    }
    if (i > 0 && buf[i - 1] == '/') {
        buf[i - 1] = fill;
    }
    buf[i] = '\0';
}

/* Long slugs map to bounded, distinct, stable slot names, and identities
 * compare whole names whatever their length (no git needed). */
TEST(branch_long_slug_slot_names) {
    char name[CBM_BRANCH_SLOT_NAME_MAX + 1];
    ASSERT_TRUE(cbm_branch_slot_name("main", name, sizeof(name)));
    ASSERT_STR_EQ(name, "main");

    static char a[1400];
    static char b[1400];
    bi_long_name(a, sizeof(a), 'q');
    snprintf(b, sizeof(b), "%s", a);
    b[sizeof(b) - 2] = 'z'; /* differs only in the last byte */
    char na[CBM_BRANCH_SLOT_NAME_MAX + 1];
    char nb[CBM_BRANCH_SLOT_NAME_MAX + 1];
    char *sa = cbm_git_branch_slug(a, false);
    char *sb = cbm_git_branch_slug(b, false);
    ASSERT_TRUE(cbm_branch_slot_name(sa, na, sizeof(na)));
    ASSERT_TRUE(cbm_branch_slot_name(sb, nb, sizeof(nb)));
    ASSERT_EQ((int)strlen(na), CBM_BRANCH_SLOT_NAME_MAX);
    ASSERT_STR_NEQ(na, nb);
    ASSERT_TRUE(cbm_branch_slot_name(sa, nb, sizeof(nb)));
    ASSERT_STR_EQ(na, nb); /* stable */

    cbm_branch_id_t ia = {};
    cbm_branch_id_t ib = {};
    ia.known = ib.known = true;
    ia.branch = strdup(a);
    ib.branch = strdup(b);
    ia.slug = sa;
    ib.slug = sb;
    ASSERT_FALSE(cbm_branch_id_same(&ia, &ib));
    cbm_branch_id_free(&ia);
    cbm_branch_id_free(&ib);
    PASS();
}

/* A slug that is a Windows device name (in any case, also with an
 * extension) never names a slot file as it is: the file would be the device.
 * The mangled name is a real file in the slot directory, distinct per slug;
 * other names are left alone. */
TEST(branch_slot_name_avoids_windows_devices) {
    const char *reserved[] = {"con",  "CON", "Nul",   "aux",     "prn.x", "com1",
                              "COM9", "lpt1", "lpt9.db", "con.feature"};
    const char *plain[] = {"main", "console", "com10", "con-x", "lpt", "nul_", "auxiliary"};
    char *dir = th_mktempdir("cbm_bi_dev");
    ASSERT_NOT_NULL(dir);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", dir);
    char names[10][CBM_BRANCH_SLOT_NAME_MAX + 1];
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(cbm_branch_slot_name(reserved[i], names[i], sizeof(names[i])));
        ASSERT_EQ(names[i][0], '_');
        ASSERT_STR_NEQ(names[i], reserved[i]);
        for (int k = 0; k < i; k++) {
            ASSERT_STR_NEQ(names[i], names[k]);
        }
        char path[600];
        snprintf(path, sizeof(path), "%s/%s.db.zst", saved, names[i]);
        ASSERT_EQ(th_write_file(path, "slot"), 0);
        ASSERT_TRUE(cbm_is_regular_file(path));
    }
    int files = 0;
    cbm_dir_t *d = cbm_opendir(saved);
    ASSERT_NOT_NULL(d);
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        files += !ent->is_dir && ent->name[0] == '_';
    }
    cbm_closedir(d);
    ASSERT_EQ(files, 10); /* ten files, none of them a device */
    for (const char *p : plain) {
        char name[CBM_BRANCH_SLOT_NAME_MAX + 1];
        ASSERT_TRUE(cbm_branch_slot_name(p, name, sizeof(name)));
        ASSERT_STR_EQ(name, p);
    }
    th_rmtree(saved);
    PASS();
}

/* Branches with ~300- and ~900-byte names (the longest this host's git can
 * create: a loose ref path must fit PATH_MAX) park, restore and repeat like
 * any other: a repeat run is not a switch, and the slot file is bounded. */
TEST(branch_long_names_park_and_restore) {
    char *repo = bi_make_repo("cbm_bi_long");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    static char longest[901];
    static char mid[301];
    bi_long_name(longest, sizeof(longest), 'l');
    bi_long_name(mid, sizeof(mid), 'm');

    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main */
    const char *names[] = {mid, longest};
    for (int i = 0; i < 2; i++) {
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "checkout -q -b %s main", names[i]);
        ASSERT_EQ(bi_git(repo, cmd), 0);
        bi_run_t r = bi_index(repo, -1);
        ASSERT_EQ(r.rc, 0);
        ASSERT_STR_EQ(r.action, "seeded");
        r = bi_index(repo, -1);
        ASSERT_EQ(r.rc, 0);
        ASSERT_STR_EQ(r.action, "none");
        ASSERT_TRUE(bi_head_matches_index(repo));
        ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
        r = bi_index(repo, -1);
        ASSERT_STR_EQ(r.action, "restored");
        snprintf(cmd, sizeof(cmd), "checkout -q %s", names[i]);
        ASSERT_EQ(bi_git(repo, cmd), 0);
        r = bi_index(repo, -1);
        ASSERT_STR_EQ(r.action, "restored");
        ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
        r = bi_index(repo, -1);
        ASSERT_STR_EQ(r.action, "restored");
    }
    char **list = NULL;
    int n = bi_parked(&list);
    for (int i = 0; i < n; i++) {
        ASSERT_TRUE(strlen(list[i]) <= CBM_BRANCH_SLOT_NAME_MAX);
    }
    cbm_branch_slots_free_list(list, n);
    ASSERT_EQ(n, 2);

    bi_cleanup(repo);
    PASS();
}

/* Index repo into the default cache database with persistence requested.
 * Only the POSIX-only export tests below use it. */
#ifndef _WIN32
static int bi_index_persist(const char *repo) {
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    if (!p) {
        return CBM_NOT_FOUND;
    }
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    return rc;
}

/* An artifact export the run was asked for fails the run, also when it is
 * the end-of-run export (a restore with nothing re-extracted); refreshing an
 * existing artifact nobody asked for stays best-effort. */
TEST(branch_requested_export_failure_fails_run) {
    if (geteuid() == 0) {
        PASS(); /* root writes through 0500 */
    }
    char *repo = bi_make_repo("cbm_bi_artfail");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char art[512];
    snprintf(art, sizeof(art), "%s/.code-cortex", repo);

    ASSERT_EQ(bi_index_persist(repo), 0); /* main, exported */
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* feature, exported; main parked */

    ASSERT_EQ(chmod(art, 0500), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    int rc = bi_index_persist(repo); /* restored, nothing re-extracted */
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    bi_run_t r = bi_index(repo, -1); /* restored, opportunistic refresh */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r2 = bi_index(repo, -1); /* restored again; export fails again */
    (void)chmod(art, 0755);
    ASSERT_NEQ(rc, 0);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r2.rc, 0);

    /* The artifact still holds feature. A later run with nothing to do (no
     * persistence flag either) repairs it, and the one after that is a true
     * no-op. */
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_EQ(r.reparsed, 0);
    char slug[256];
    ASSERT_EQ(bi_artifact_probe(repo, "only_main", slug, sizeof(slug)), 1);
    ASSERT_STR_EQ(slug, "main");
    int64_t before = bi_artifact_mtime(repo);
    cbm_usleep(1100000);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_artifact_mtime(repo), before);

    bi_cleanup(repo);
    PASS();
}
#endif

/* A git repo at exactly `path` on main with one file defining `fn`. */
static bool bi_repo_at(const char *path, const char *fn) {
    if (!cbm_mkdir_p(path, 0755) || bi_git(path, "init -q -b main") != 0) {
        return false;
    }
    char file[1024];
    snprintf(file, sizeof(file), "%s/src/m.py", path);
    char body[256];
    snprintf(body, sizeof(body), "def %s():\n    return 1\n", fn);
    th_write_file(file, body);
    return bi_git(path, "add .") == 0 && bi_git(path, "commit -q -m init") == 0;
}

/* The root recorded in checkout root's parked slot `slug`; "" when
 * unreadable. */
static void bi_slot_root(const char *root, const char *slug, char *buf, size_t n) {
    buf[0] = '\0';
    char db[1024];
    char dir[1100];
    char slot[1300];
    if (!cbm_branch_slots_dir(bi_db_path(db, sizeof(db)), g_bi_project, root, dir, sizeof(dir))) {
        return;
    }
    snprintf(slot, sizeof(slot), "%s/%s.db.zst", dir, slug);
    char *tmp = th_mktempdir("cbm_bi_unpack");
    char unpacked[1300];
    snprintf(unpacked, sizeof(unpacked), "%s/slot.db", tmp ? tmp : "");
    cbm_store_t *s = tmp && cbm_branch_slot_decompress(slot, unpacked)
                         ? cbm_store_open_path_query(unpacked)
                         : NULL;
    cbm_project_t proj = {0};
    if (s && cbm_store_get_project(s, g_bi_project, &proj) == CBM_STORE_OK) {
        snprintf(buf, n, "%s", proj.root_path ? proj.root_path : "");
        cbm_project_free_fields(&proj);
    }
    cbm_store_close(s);
    if (tmp) {
        th_rmtree(tmp);
    }
}

/* Two checkouts whose paths derive the same project name share its slots
 * directory: neither may install, nor park over, the other's database. */
TEST(branch_slots_reject_other_roots) {
    char *made = th_mktempdir("cbm_bi_collide");
    ASSERT_NOT_NULL(made);
    char base[256];
    snprintf(base, sizeof(base), "%s", made);
    char a[512];
    char b[512];
    snprintf(a, sizeof(a), "%s/a/b-repo", base);
    snprintf(b, sizeof(b), "%s/a-b/repo", base);
    ASSERT_TRUE(bi_repo_at(a, "from_a"));
    ASSERT_TRUE(bi_repo_at(b, "from_b"));
    char root[1024];
    char adr[256];
    char db[1024];

    ASSERT_EQ(bi_index(a, -1).rc, 0); /* A on main */
    char *name_b = cbm_project_name_from_path(b);
    ASSERT_STR_EQ(name_b, g_bi_project); /* the collision */
    free(name_b);
    ASSERT_EQ(bi_adr_put("A-ADR"), CBM_STORE_OK);
    ASSERT_EQ(bi_git(a, "checkout -q -b feature"), 0);
    ASSERT_EQ(bi_index(a, -1).rc, 0); /* A's main parked */
    bi_slot_root(a, "main", root, sizeof(root));
    ASSERT_STR_EQ(root, a);

    /* Live database gone; B (also on main) must not install A's slot. */
    bi_db_path(db, sizeof(db));
    ASSERT_EQ(cbm_unlink(db), 0);
    cbm_remove_db_sidecars(db);
    bi_run_t r = bi_index(b, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "");
    ASSERT_EQ(bi_count("from_b"), 1);
    ASSERT_EQ(bi_count("from_a"), 0);
    char **none = NULL;
    int nb = bi_parked(&none); /* B's own slot directory */
    cbm_branch_slots_free_list(none, nb);
    ASSERT_EQ(nb, 0);
    bi_slot_root(a, "main", root, sizeof(root));
    ASSERT_STR_EQ(root, a); /* A's slot, untouched */

    /* The live database is now B's. A on feature must not park it as its
     * "main" (over A's own slot); the run reindexes over it, as before. */
    r = bi_index(a, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "none");
    bi_slot_root(a, "main", root, sizeof(root));
    ASSERT_STR_EQ(root, a);
    char **list = NULL;
    int n = bi_parked(&list);
    cbm_branch_slots_free_list(list, n);
    ASSERT_EQ(n, 1);

    /* B switching branches parks its own main -- in its own directory, not
     * over A's slot of the same name. */
    ASSERT_EQ(bi_git(b, "checkout -q -b other"), 0);
    ASSERT_EQ(bi_index(b, -1).rc, 0); /* live is A's: no switch, reindexed */
    ASSERT_EQ(bi_git(b, "checkout -q main"), 0);
    ASSERT_EQ(bi_index(b, -1).rc, 0);
    ASSERT_EQ(bi_git(b, "checkout -q other"), 0);
    r = bi_index(b, -1);
    ASSERT_STR_EQ(r.action, "restored"); /* B's own "other" */
    bi_slot_root(b, "main", root, sizeof(root));
    ASSERT_STR_EQ(root, b);
    bi_slot_root(a, "main", root, sizeof(root));
    ASSERT_STR_EQ(root, a);

    (void)cbm_branch_slots_delete_all(db, g_bi_project);
    cbm_unlink(db);
    cbm_remove_db_sidecars(db);
    th_rmtree(base);
    PASS();
}

/* delete_project for one of two checkouts that derive the same project name
 * deletes only what is that checkout's: its parked slots, and the shared
 * database only when that checkout indexed it last. */
static int bi_slot_count(const char *root) {
    char db[1024];
    char **list = NULL;
    int n = cbm_branch_slots_list(bi_db_path(db, sizeof(db)), g_bi_project, root, &list);
    cbm_branch_slots_free_list(list, n);
    return n;
}

static char *bi_delete_by_path(cbm_mcp_server_t *srv, const char *dir) {
    char fwd[600];
    snprintf(fwd, sizeof(fwd), "%s", dir);
    for (char *p = fwd; *p; p++) {
        *p = *p == '\\' ? '/' : *p; /* no JSON escaping needed */
    }
    char args[800];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", fwd);
    return cbm_mcp_handle_tool(srv, "delete_project", args);
}

TEST(branch_delete_project_keeps_other_roots) {
    char *made = th_mktempdir("cbm_bi_delcoll");
    ASSERT_NOT_NULL(made);
    char base[256];
    snprintf(base, sizeof(base), "%s", made);
    char a[512];
    char b[512];
    snprintf(a, sizeof(a), "%s/a/b-repo", base);
    snprintf(b, sizeof(b), "%s/a-b/repo", base);
    ASSERT_TRUE(bi_repo_at(a, "from_a"));
    ASSERT_TRUE(bi_repo_at(b, "from_b"));
    char ra[1024];
    char rb[1024];

    /* B parks its main, then A indexes over B's live database and parks
     * its own main: both roots have a slot, and the live database is A's. */
    ASSERT_EQ(bi_index(b, -1).rc, 0);
    snprintf(rb, sizeof(rb), "%s", g_bi_root);
    ASSERT_EQ(bi_git(b, "checkout -q -b other"), 0);
    ASSERT_EQ(bi_index(b, -1).rc, 0);
    ASSERT_EQ(bi_index(a, -1).rc, 0);
    snprintf(ra, sizeof(ra), "%s", g_bi_root);
    ASSERT_EQ(bi_git(a, "checkout -q -b feature"), 0);
    ASSERT_EQ(bi_index(a, -1).rc, 0);
    ASSERT_EQ(bi_slot_count(ra), 1);
    ASSERT_EQ(bi_slot_count(rb), 1);
    char db[1024];
    bi_db_path(db, sizeof(db));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    /* Deleting B: A's live database stays, B's slot goes, A's stays. */
    char *resp = bi_delete_by_path(srv, b);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "kept_database_of"));
    free(resp);
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(bi_count("from_a"), 1);
    ASSERT_EQ(bi_slot_count(rb), 0);
    ASSERT_EQ(bi_slot_count(ra), 1);

    /* B's slot again; deleting A takes the database and A's slot only. */
    ASSERT_EQ(bi_git(b, "checkout -q main"), 0);
    ASSERT_EQ(bi_index(b, -1).rc, 0); /* over A's database: no switch */
    ASSERT_EQ(bi_git(b, "checkout -q other"), 0);
    ASSERT_EQ(bi_index(b, -1).rc, 0); /* B's main parked */
    ASSERT_EQ(bi_index(a, -1).rc, 0); /* the live database is A's again */
    ASSERT_EQ(bi_slot_count(rb), 1);
    resp = bi_delete_by_path(srv, a);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"status\\\":\\\"deleted\\\""));
    free(resp);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_EQ(bi_slot_count(ra), 0);
    ASSERT_EQ(bi_slot_count(rb), 1);

    /* By bare name, with no database left: every root's slots go. */
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", g_bi_project);
    resp = cbm_mcp_handle_tool(srv, "delete_project", args);
    free(resp);
    ASSERT_EQ(bi_slot_count(rb), 0);
    cbm_mcp_server_free(srv);

    (void)cbm_branch_slots_delete_all(db, g_bi_project);
    cbm_unlink(db);
    cbm_remove_db_sidecars(db);
    th_rmtree(base);
    PASS();
}

/* delete_project by path keeps a database whose owner cannot be read: with
 * colliding derived names it may be another directory's. By bare name it
 * still goes -- the way to remove a corrupt index. */
TEST(branch_delete_by_path_keeps_unreadable_owner) {
    char *repo = bi_make_repo("cbm_bi_delunk");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    char db[1024];
    bi_db_path(db, sizeof(db));
    cbm_remove_db_sidecars(db);
    ASSERT_EQ(th_write_file(db, "this is not an sqlite database, its owner is unknowable"), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = bi_delete_by_path(srv, repo);
    ASSERT_NOT_NULL(resp);
    bool kept_note = strstr(resp, "could not be read") != NULL;
    free(resp);
    bool kept = cbm_file_exists(db);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", g_bi_project);
    resp = cbm_mcp_handle_tool(srv, "delete_project", args);
    free(resp);
    bool gone = !cbm_file_exists(db);
    cbm_mcp_server_free(srv);

    ASSERT_TRUE(kept_note);
    ASSERT_TRUE(kept);
    ASSERT_TRUE(gone);
    bi_cleanup(repo);
    PASS();
}

/* Root normalization is a pure string function, so Windows spellings are
 * checked on every platform; the comparison and the slot key build on it. */
TEST(branch_root_normalize_and_key) {
    char out[256];
#ifdef _WIN32
    ASSERT_TRUE(cbm_root_normalize("C:\\Users\\Me\\repo\\", true, out, sizeof(out)));
    ASSERT_STR_EQ(out, "c:/users/me/repo");
    ASSERT_TRUE(cbm_root_normalize("c:/Users/Me/./x/../repo", true, out, sizeof(out)));
    ASSERT_STR_EQ(out, "c:/users/me/repo");
    ASSERT_TRUE(cbm_root_normalize("C:\\", true, out, sizeof(out)));
    ASSERT_STR_EQ(out, "c:/");
    ASSERT_TRUE(cbm_root_normalize("\\\\srv\\share\\x\\", true, out, sizeof(out)));
    ASSERT_STR_EQ(out, "//srv/share/x");
#else
    /* POSIX has no drives, shares or backslash separators: distinct paths
     * stay distinct. */
    ASSERT_TRUE(cbm_root_normalize("/tmp/a\\b/repo", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/tmp/a\\b/repo");
    ASSERT_TRUE(cbm_root_normalize("C:/Repo", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "C:/Repo");
    ASSERT_TRUE(cbm_root_normalize("//x/y", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/x/y");
#endif
    ASSERT_TRUE(cbm_root_normalize("/a/./b//../c/", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/a/c");
    ASSERT_TRUE(cbm_root_normalize("/..", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/");
    ASSERT_TRUE(cbm_root_normalize("/A/B", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/A/B"); /* POSIX keeps case */

    /* Trailing separators, "."/"..", and a recorded root that no longer
     * exists still match their directory. */
    char *dir = th_mktempdir("cbm_bi_rootcmp");
    ASSERT_NOT_NULL(dir);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", dir);
    char spelled[512];
    snprintf(spelled, sizeof(spelled), "%s/./sub/../", saved);
    ASSERT_TRUE(cbm_root_matches(spelled, saved));
    ASSERT_TRUE(cbm_root_matches("/nonexistent/cbm/x/", "/nonexistent/cbm/x"));
    ASSERT_FALSE(cbm_root_matches("/nonexistent/cbm/x", "/nonexistent/cbm/y"));
    char k1[32];
    char k2[32];
    ASSERT_TRUE(cbm_branch_root_key(saved, k1, sizeof(k1)));
    ASSERT_TRUE(cbm_branch_root_key(spelled, k2, sizeof(k2)));
    ASSERT_STR_EQ(k1, k2);
    th_rmtree(saved);
    PASS();
}

/* A slot an earlier build parked directly in the project directory is
 * adopted when it records this checkout, and used. */
TEST(branch_legacy_slot_adopted) {
    char *repo = bi_make_repo("cbm_bi_legacy_slot");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked (namespaced) */

    char db[1024];
    char ns[1100];
    char flat[1100];
    char from[1300];
    char to[1300];
    bi_db_path(db, sizeof(db));
    ASSERT_TRUE(cbm_branch_slots_dir(db, g_bi_project, repo, ns, sizeof(ns)));
    ASSERT_TRUE(cbm_branch_slots_dir(db, g_bi_project, NULL, flat, sizeof(flat)));
    snprintf(from, sizeof(from), "%s/main.db.zst", ns);
    snprintf(to, sizeof(to), "%s/main.db", flat);
    /* The old, shared layout, from before slots were compressed. */
    ASSERT_TRUE(cbm_branch_slot_decompress(from, to));
    ASSERT_EQ(cbm_unlink(from), 0);

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_FALSE(cbm_file_exists(to));

    bi_cleanup(repo);
    PASS();
}

/* A symlink in a legacy slot's place is not adopted: it stays where it is,
 * nothing is restored from it, and the restore's ADR carry never writes into
 * the database it points at. */
TEST(branch_legacy_slot_link_not_adopted) {
#ifndef _WIN32
    char *repo = bi_make_repo("cbm_bi_legacy_link");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked (namespaced) */

    char db[1024];
    char ns[1100];
    char flat[1100];
    char from[1300];
    char link[1300];
    char outside[600];
    bi_db_path(db, sizeof(db));
    ASSERT_TRUE(cbm_branch_slots_dir(db, g_bi_project, repo, ns, sizeof(ns)));
    ASSERT_TRUE(cbm_branch_slots_dir(db, g_bi_project, NULL, flat, sizeof(flat)));
    snprintf(from, sizeof(from), "%s/main.db.zst", ns);
    snprintf(link, sizeof(link), "%s/main.db", flat);
    char *odir = th_mktempdir("cbm_bi_outside");
    ASSERT_NOT_NULL(odir);
    char outdir[256];
    snprintf(outdir, sizeof(outdir), "%s", odir);
    snprintf(outside, sizeof(outside), "%s/outside.db", outdir);
    ASSERT_TRUE(cbm_branch_slot_decompress(from, outside));
    ASSERT_EQ(cbm_unlink(from), 0);
    ASSERT_EQ(symlink(outside, link), 0);
    ASSERT_EQ(bi_adr_put("LIVE-ADR"), CBM_STORE_OK); /* the carry would write it */

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_NEQ(r.action, "restored");
    ASSERT_TRUE(cbm_is_link(link)); /* left in place */
    cbm_store_t *s = cbm_store_open_path_query(outside);
    ASSERT_NOT_NULL(s);
    cbm_adr_t adr = {0};
    ASSERT_EQ(cbm_store_adr_get(s, g_bi_project, &adr), CBM_STORE_NOT_FOUND);
    cbm_store_close(s);
    (void)cbm_unlink(link);
    th_rmtree(outdir);
    bi_cleanup(repo);
#endif
    PASS();
}

/* Index repo in `mode` into the default cache database. */
static bi_run_t bi_index_mode(const char *repo, cbm_index_mode_t mode) {
    bi_run_t r = {CBM_NOT_FOUND, "", "", -1};
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, mode);
    if (!p) {
        return r;
    }
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    r.rc = cbm_pipeline_run(p);
    const char *action = NULL;
    cbm_pipeline_get_branch_switch(p, &action, NULL, NULL, &r.reparsed);
    snprintf(r.action, sizeof(r.action), "%s", action ? action : "");
    cbm_pipeline_free(p);
    return r;
}

/* After a switch, a stored file the current mode does not discover (docs/
 * in fast mode) cannot be re-extracted; when its bytes differ on the new
 * branch it is purged rather than kept with the old branch's symbols. */
TEST(branch_switch_purges_changed_mode_skipped_file) {
    char *repo = bi_make_repo("cbm_bi_modeskip");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    th_write_file(TH_PATH(repo, "docs/x.py"), "def doc_main():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m docs"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q -b docsonly"), 0);
    th_write_file(TH_PATH(repo, "docs/x.py"), "def doc_feat():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "commit -q -am docs2"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);

    ASSERT_EQ(bi_index_mode(repo, CBM_MODE_FULL).rc, 0);
    ASSERT_EQ(bi_count("doc_main"), 1);
    ASSERT_EQ(bi_git(repo, "checkout -q docsonly"), 0);
    bi_run_t r = bi_index_mode(repo, CBM_MODE_FAST);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("doc_main"), 0);
    ASSERT_EQ(bi_count("only_main"), 1); /* the rest of the tree is kept */

    bi_cleanup(repo);
    PASS();
}

/* Rows an older build stored for the artifact directory (.code-cortex/,
 * which discovery now skips) are purged by the next incremental run, even
 * though the file is still on disk. */
static int bi_artifact_rows(int *hashes) {
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    if (!s) {
        return -1;
    }
    cbm_node_t *nodes = NULL;
    int count = 0;
    cbm_store_find_nodes_by_file(s, g_bi_project, ".code-cortex/artifact.json", &nodes, &count);
    cbm_store_free_nodes(nodes, count);
    cbm_file_hash_t *fh = NULL;
    int nfh = 0;
    cbm_store_get_file_hashes(s, g_bi_project, &fh, &nfh);
    *hashes = 0;
    for (int i = 0; i < nfh; i++) {
        *hashes += strcmp(fh[i].rel_path, ".code-cortex/artifact.json") == 0;
    }
    cbm_store_free_file_hashes(fh, nfh);
    cbm_store_close(s);
    return count;
}

TEST(incremental_purges_stale_artifact_dir_rows) {
    char *repo = bi_make_repo("cbm_bi_artstale");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index_mode(repo, CBM_MODE_FULL).rc, 0);
    th_write_file(TH_PATH(repo, ".code-cortex/artifact.json"), "{\"version\": 1}\n");

    char db[1024];
    cbm_store_t *s = cbm_store_open_path(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    char qn[1200];
    snprintf(qn, sizeof(qn), "%s..code-cortex.artifact.json", g_bi_project);
    cbm_node_t n = {0};
    n.project = g_bi_project;
    n.label = "File";
    n.name = "artifact.json";
    n.qualified_name = qn;
    n.file_path = ".code-cortex/artifact.json";
    ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    char zeros[65];
    memset(zeros, '0', 64);
    zeros[64] = '\0';
    ASSERT_EQ(cbm_store_upsert_file_hash(s, g_bi_project, ".code-cortex/artifact.json", zeros, 1,
                                         15),
              CBM_STORE_OK);
    cbm_store_close(s);
    int hashes = 0;
    ASSERT_EQ(bi_artifact_rows(&hashes), 1);
    ASSERT_EQ(hashes, 1);

    ASSERT_EQ(bi_index_mode(repo, CBM_MODE_FULL).rc, 0);
    ASSERT_EQ(bi_artifact_rows(&hashes), 0);
    ASSERT_EQ(hashes, 0);
    ASSERT_EQ(bi_count("only_main"), 1); /* the rest of the graph is kept */

    bi_cleanup(repo);
    PASS();
}

/* A live database another checkout indexed with the same branch, commit and
 * bytes is still not this checkout's index: the run rebuilds it, so it
 * records this root. */
TEST(branch_foreign_live_db_same_identity_rebuilds) {
    char *made = th_mktempdir("cbm_bi_foreign");
    ASSERT_NOT_NULL(made);
    char base[256];
    snprintf(base, sizeof(base), "%s", made);
    char a[512];
    char b[512];
    snprintf(a, sizeof(a), "%s/a/b-repo", base);
    snprintf(b, sizeof(b), "%s/a-b/repo", base);
    ASSERT_TRUE(bi_repo_at(a, "from_a"));
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "clone -q \"%s\" \"%s\"", a, b);
    ASSERT_EQ(bi_git(base, cmd), 0); /* same branch, HEAD and bytes */

    ASSERT_EQ(bi_index(a, -1).rc, 0);
    ASSERT_EQ(bi_adr_put("A-ADR"), CBM_STORE_OK);
    bi_run_t r = bi_index(b, -1);
    ASSERT_EQ(r.rc, 0);
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    ASSERT_NOT_NULL(s);
    cbm_project_t proj = {0};
    ASSERT_EQ(cbm_store_get_project(s, g_bi_project, &proj), CBM_STORE_OK);
    bool mine = cbm_root_matches(proj.root_path, b) && !cbm_root_matches(proj.root_path, a);
    cbm_project_free_fields(&proj);
    cbm_store_close(s);
    ASSERT_TRUE(mine);
    char adr[256];
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), ""); /* A's ADR is not B's */

    bi_cleanup(b);
    th_rmtree(base);
    PASS();
}

/* Run one SQL statement on the project's live database. */
static bool bi_db_exec(const char *sql) {
    char db[1024];
    sqlite3 *h = NULL;
    bool ok = sqlite3_open(bi_db_path(db, sizeof(db)), &h) == SQLITE_OK &&
              sqlite3_exec(h, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_close(h);
    return ok;
}

#ifndef _WIN32
/* A rename that keeps node and edge counts, made while the export fails, is
 * still caught by a later no-op run: the artifact is compared by content. */
TEST(branch_artifact_behind_after_count_preserving_edit) {
    if (geteuid() == 0) {
        PASS();
    }
    char *repo = bi_make_repo("cbm_bi_artfp");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char art[512];
    snprintf(art, sizeof(art), "%s/.code-cortex", repo);
    ASSERT_EQ(bi_index_persist(repo), 0);

    ASSERT_EQ(chmod(art, 0500), 0);
    th_write_file(TH_PATH(repo, "src/common.py"), "def common_helpex():\n    return 2\n");
    bi_run_t r = bi_index(repo, -1); /* re-extracts; its export fails */
    (void)chmod(art, 0755);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 1);

    r = bi_index(repo, -1); /* nothing to do, but the artifact is behind */
    ASSERT_EQ(r.reparsed, 0);
    char slug[256];
    ASSERT_EQ(bi_artifact_probe(repo, "common_helpex", slug, sizeof(slug)), 1);

    bi_cleanup(repo);
    PASS();
}

/* A rebuild from unchanged sources (an index format upgrade) whose export
 * fails leaves the Branch node and the content fingerprint as the artifact
 * recorded them; the graph generation still tells a later no-op run that the
 * artifact is behind. */
TEST(branch_artifact_behind_after_format_rebuild) {
    if (geteuid() == 0) {
        PASS();
    }
    char *repo = bi_make_repo("cbm_bi_artfmt");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char art[512];
    snprintf(art, sizeof(art), "%s/.code-cortex", repo);
    char db[1024];
    ASSERT_EQ(bi_index_persist(repo), 0);
    bi_db_path(db, sizeof(db));
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    /* The graph and its artifact as an older build wrote them. */
    ASSERT_TRUE(bi_db_exec("PRAGMA user_version = 3;"));
    ASSERT_EQ(cbm_artifact_export(db, repo, g_bi_project, CBM_ARTIFACT_FAST), 0);
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    ASSERT_EQ(chmod(art, 0500), 0);
    bi_run_t r = bi_index(repo, -1); /* format rebuild; its export fails */
    (void)chmod(art, 0755);
    ASSERT_EQ(r.rc, 0);
    ASSERT_FALSE(cbm_artifact_is_current(db, repo, g_bi_project));

    int64_t before = bi_artifact_mtime(repo);
    r = bi_index(repo, -1); /* nothing to do, but the artifact is behind */
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_NEQ(bi_artifact_mtime(repo), before);
    ASSERT_TRUE(cbm_artifact_is_current(db, repo, g_bi_project));

    /* Another dump of the same sources moves the generation. */
    ASSERT_TRUE(bi_db_exec("UPDATE projects SET indexed_at = '2000-01-01T00:00:00Z';"));
    ASSERT_FALSE(cbm_artifact_is_current(db, repo, g_bi_project));

    bi_cleanup(repo);
    PASS();
}

/* delete_project and the index reset keep a project's parked branches when
 * its live database could not be removed. */
TEST(branch_slots_kept_when_live_db_undeletable) {
    if (geteuid() == 0) {
        PASS();
    }
    char *repo = bi_make_repo("cbm_bi_undel");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked */
    ASSERT_TRUE(bi_is_parked("main"));

    const char *cache = cbm_resolve_cache_dir();
    char cache_dir[1024];
    snprintf(cache_dir, sizeof(cache_dir), "%s", cache);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[1200];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", g_bi_project);
    ASSERT_EQ(chmod(cache_dir, 0500), 0); /* the live .db cannot be unlinked */
    char *resp = cbm_mcp_handle_tool(srv, "delete_project", args);
    int reset_removed = cbm_remove_indexes(NULL);
    (void)chmod(cache_dir, 0755);
    ASSERT_NOT_NULL(resp);
    bool failed = strstr(resp, "delete_failed") != NULL;
    free(resp);
    cbm_mcp_server_free(srv);
    ASSERT_TRUE(failed);
    ASSERT_EQ(reset_removed, 0);
    ASSERT_TRUE(bi_is_parked("main"));

    bi_cleanup(repo);
    PASS();
}
#endif

#ifndef _WIN32
/* A team artifact exported from another clone under the same explicit
 * project name is adopted by the checkout that bootstraps from it: no
 * foreign-index rebuild, its ADR kept, and every ownership check answers for
 * the importing checkout. */
TEST(branch_artifact_bootstrap_adopted_by_importer) {
    char *made = th_mktempdir("cbm_bi_boot");
    ASSERT_NOT_NULL(made);
    char base[256];
    snprintf(base, sizeof(base), "%s", made);
    char a[512];
    char b[512];
    snprintf(a, sizeof(a), "%s/exporter", base);
    snprintf(b, sizeof(b), "%s/importer", base);
    ASSERT_TRUE(bi_repo_at(a, "team_fn"));

    cbm_pipeline_t *p = cbm_pipeline_new(a, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(cbm_pipeline_set_project_name(p, "teamproj"));
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", "teamproj");
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    ASSERT_EQ(bi_adr_put("TEAM"), CBM_STORE_OK);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_EQ(cbm_artifact_export(db, a, "teamproj", CBM_ARTIFACT_FAST), 0);

    /* Another clone, with the committed-alongside artifact; no local index. */
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "clone -q \"%s\" \"%s\"", a, b);
    ASSERT_EQ(bi_git(base, cmd), 0);
    snprintf(cmd, sizeof(cmd), "cp -R \"%s/.code-cortex\" \"%s/\"", a, b);
    ASSERT_EQ(system(cmd), 0);
    ASSERT_EQ(cbm_unlink(db), 0);
    cbm_remove_db_sidecars(db);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[1200];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"name\":\"teamproj\"}", b);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    bool reused = strstr(resp, "\\\"files_reparsed\\\":0") != NULL;
    free(resp);
    ASSERT_TRUE(reused); /* incremental over the imported graph */
    char adr[256];
    ASSERT_STR_EQ(bi_adr(adr, sizeof(adr)), "TEAM");
    ASSERT_EQ(bi_count("team_fn"), 1);
    cbm_store_t *s = cbm_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    cbm_project_t proj = {0};
    ASSERT_EQ(cbm_store_get_project(s, "teamproj", &proj), CBM_STORE_OK);
    bool mine = cbm_root_matches(proj.root_path, b) && !cbm_root_matches(proj.root_path, a);
    cbm_project_free_fields(&proj);
    cbm_store_close(s);
    ASSERT_TRUE(mine);
    snprintf(args, sizeof(args), "{\"project\":\"teamproj\"}");
    resp = cbm_mcp_handle_tool(srv, "index_status", args);
    ASSERT_NOT_NULL(resp);
    bool matches = strstr(resp, "\\\"index_matches_checkout\\\":true") != NULL;
    free(resp);
    cbm_mcp_server_free(srv);
    ASSERT_TRUE(matches);

    (void)cbm_branch_slots_delete_all(db, "teamproj");
    cbm_unlink(db);
    cbm_remove_db_sidecars(db);
    th_rmtree(base);
    PASS();
}
#endif

/* Session start, legacy and unreadable cases: a Branch node with no recorded
 * commit marks the session pending once (HEAD readable from files) and the
 * run that follows clears it; HEAD the file reader cannot read never marks
 * pending. */
TEST(branch_session_pending_legacy_once) {
    char *repo = bi_make_repo("cbm_bi_sess");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_FALSE(cbm_mcp_checkout_differs_from_index(repo, g_bi_project, db));

    char qn[1024];
    cbm_store_t *s = cbm_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_get_branch(s, g_bi_project, qn, sizeof(qn), NULL, 0), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "main", "{}"), CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT_TRUE(cbm_mcp_checkout_differs_from_index(repo, g_bi_project, db));
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_FALSE(cbm_mcp_checkout_differs_from_index(repo, g_bi_project, db));

    /* A ref store the file reader does not read (reftable): never pending. */
    s = cbm_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_set_branch(s, g_bi_project, qn, "main", "{}"), CBM_STORE_OK);
    cbm_store_close(s);
    char reftable[512];
    snprintf(reftable, sizeof(reftable), "%s/.git/reftable", repo);
    ASSERT_TRUE(cbm_mkdir_p(reftable, 0755));
    bool pending = cbm_mcp_checkout_differs_from_index(repo, g_bi_project, db);
    (void)cbm_rmdir(reftable);
    ASSERT_FALSE(pending);

    bi_cleanup(repo);
    PASS();
}

/* A branch with the name of a tag: `git symbolic-ref --short` prints
 * "heads/v1.2" for it, the HEAD file says "refs/heads/v1.2". The index run
 * and the file reader must name it the same, or every session start says the
 * index is for another branch. */
TEST(branch_name_shared_with_tag_matches_head_file) {
    char *repo = bi_make_repo("cbm_bi_tagname");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_git(repo, "tag v1.2"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q -b v1.2"), 0);

    cbm_git_context_t ctx;
    ASSERT_EQ(cbm_git_context_resolve(repo, &ctx), 0);
    char ctx_branch[128];
    snprintf(ctx_branch, sizeof(ctx_branch), "%s", ctx.branch ? ctx.branch : "");
    cbm_git_context_free(&ctx);
    ASSERT_STR_EQ(ctx_branch, "v1.2");
    cbm_git_head_t head = {0};
    ASSERT_EQ(cbm_git_head_read(repo, &head), 0);
    char head_branch[128];
    snprintf(head_branch, sizeof(head_branch), "%s", head.branch ? head.branch : "");
    cbm_git_head_free(&head);
    ASSERT_STR_EQ(head_branch, "v1.2");

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_FALSE(cbm_mcp_checkout_differs_from_index(repo, g_bi_project, db));

    bi_cleanup(repo);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* SessionStart after a checkout made while no server ran: the stored brief
 * describes the indexed branch, so the text says the checkout moved, naming
 * both sides, until an index run catches up. Soak finding: the brief used to
 * read as current. */
TEST(branch_session_start_notes_moved_checkout) {
    char *repo = bi_make_repo("cbm_bi_ssnote");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    char *text = cbm_session_start_text_for_testing(repo);
    ASSERT_NOT_NULL(text);
    bool brief = strstr(text, "this repository is indexed as") != NULL;
    bool quiet = strstr(text, "this index was built for") == NULL;
    free(text);
    ASSERT_TRUE(brief);
    ASSERT_TRUE(quiet);

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    text = cbm_session_start_text_for_testing(repo);
    ASSERT_NOT_NULL(text);
    brief = strstr(text, "this repository is indexed as") != NULL;
    bool names_indexed = strstr(text, "this index was built for branch `main` at ") != NULL;
    bool names_checkout = strstr(text, "the checkout is now branch `feature` at ") != NULL;
    free(text);
    ASSERT_TRUE(brief);
    ASSERT_TRUE(names_indexed);
    ASSERT_TRUE(names_checkout);

    ASSERT_EQ(bi_git(repo, "checkout -q --detach main"), 0);
    text = cbm_session_start_text_for_testing(repo);
    ASSERT_NOT_NULL(text);
    /* Same commit as indexed, but detached: still another checkout. */
    bool detached = strstr(text, "the checkout is now a detached HEAD at ") != NULL;
    free(text);
    ASSERT_TRUE(detached);

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    text = cbm_session_start_text_for_testing(repo);
    ASSERT_NOT_NULL(text);
    quiet = strstr(text, "this index was built for") == NULL;
    free(text);
    ASSERT_TRUE(quiet);

    bi_cleanup(repo);
    PASS();
}
#endif

/* ── Compressed slots, size budget ─────────────────────────────── */

/* Index repo with explicit parking limits. */
static bi_run_t bi_index_lim(const char *repo, int keep, int64_t budget) {
    bi_run_t r = {CBM_NOT_FOUND, "", "", -1};
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    if (!p) {
        return r;
    }
    cbm_pipeline_set_branch_keep(p, keep);
    cbm_pipeline_set_branch_budget(p, budget);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    r.rc = cbm_pipeline_run(p);
    const char *action = NULL;
    cbm_pipeline_get_branch_switch(p, &action, NULL, NULL, &r.reparsed);
    snprintf(r.action, sizeof(r.action), "%s", action ? action : "");
    cbm_pipeline_free(p);
    return r;
}

/* SHA-256 over the project's sorted nodes (label, QN, file, lines) and edges
 * (type, source QN, target QN, properties): equal digests mean the same
 * graph. "" when unreadable. */
static const char *bi_graph_sig(char *buf, size_t n) {
    buf[0] = '\0';
    char db[1024];
    sqlite3 *h = NULL;
    if (sqlite3_open_v2(bi_db_path(db, sizeof(db)), &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(h);
        return buf;
    }
    const char *sql[] = {
        "SELECT label||'|'||qualified_name||'|'||file_path||'|'||start_line||'|'||end_line "
        "FROM nodes WHERE project=?1 ORDER BY 1",
        "SELECT e.type||'|'||a.qualified_name||'|'||b.qualified_name||'|'||"
        "coalesce(e.properties,'') FROM edges e JOIN nodes a ON a.id=e.source_id "
        "JOIN nodes b ON b.id=e.target_id WHERE e.project=?1 ORDER BY 1"};
    size_t cap = 1 << 16;
    size_t len = 0;
    char *all = (char *)malloc(cap);
    for (int q = 0; all && q < 2; q++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(h, sql[q], -1, &st, NULL) != SQLITE_OK) {
            free(all);
            all = NULL;
            break;
        }
        sqlite3_bind_text(st, 1, g_bi_project, -1, SQLITE_STATIC);
        while (all && sqlite3_step(st) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0);
            size_t tl = t ? strlen(t) : 0;
            while (len + tl + 2 > cap) {
                cap *= 2;
                char *g = (char *)realloc(all, cap);
                if (!g) {
                    free(all);
                    all = NULL;
                    break;
                }
                all = g;
            }
            if (all) {
                memcpy(all + len, t ? t : "", tl);
                len += tl;
                all[len++] = '\n';
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(h);
    if (all && n > CBM_SHA256_HEX_LEN) {
        cbm_sha256_hex(all, len, buf);
    }
    free(all);
    return buf;
}

/* <slots dir of the last-indexed root>/<slug><ext>. */
static const char *bi_slot_file(const char *slug, const char *ext, char *buf, size_t n) {
    char db[1024];
    char dir[1100];
    buf[0] = '\0';
    if (cbm_branch_slots_dir(bi_db_path(db, sizeof(db)), g_bi_project, g_bi_root, dir,
                             sizeof(dir))) {
        snprintf(buf, n, "%s/%s%s", dir, slug, ext);
    }
    return buf;
}

static int64_t bi_fsize(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* The whole file, or "" when it cannot be read. */
static std::string bi_file_bytes(const char *path) {
    std::string out;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return out;
}

/* A switch whose closing branch stamp cannot be written fails the run, and
 * the database -- now feature's graph under main's name -- is never parked
 * as main by the next run on a third branch: main's slot keeps main's graph. */
TEST(branch_failed_stamp_fails_run_and_blocks_park) {
    char *repo = bi_make_repo("cbm_bi_stampfail");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    cbm_pipeline_test_fail_stamp(true);
    bi_run_t failed = bi_index(repo, -1);
    cbm_pipeline_test_fail_stamp(false);
    char db[1024];
    char marker[1100];
    snprintf(marker, sizeof(marker), "%s.stamp-pending", bi_db_path(db, sizeof(db)));
    bool marked = cbm_file_exists(marker);
    char slot[1300];
    bi_slot_file("main", ".db.zst", slot, sizeof(slot));
    std::string main_slot = bi_file_bytes(slot);

    ASSERT_EQ(bi_git(repo, "checkout -q -b other"), 0);
    bi_run_t next = bi_index(repo, -1);
    std::string main_after = bi_file_bytes(slot);
    bool marker_left = cbm_file_exists(marker);
    int only_feature = bi_count("only_feature");

    ASSERT_NEQ(failed.rc, 0);
    ASSERT_TRUE(marked);
    ASSERT_FALSE(main_slot.empty());
    ASSERT_EQ(next.rc, 0);
    ASSERT_TRUE(main_after == main_slot);
    ASSERT_FALSE(marker_left);
    ASSERT_EQ(only_feature, 1); /* other is feature's tree */

    bi_cleanup(repo);
    PASS();
}
#endif

#ifdef CBM_ENABLE_TEST_SEAMS
#ifndef _WIN32
/* A failed stamp whose marker cannot be written either removes the database:
 * nothing may be left that the next run could park under the stale label.
 * The squatter is a dangling symlink at the marker's name: it does not
 * exist for the park check, and it cannot be opened for writing. */
TEST(branch_failed_stamp_without_marker_removes_db) {
    char *repo = bi_make_repo("cbm_bi_stampnomark");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    char db[1024];
    char marker[1100];
    snprintf(marker, sizeof(marker), "%s.stamp-pending", bi_db_path(db, sizeof(db)));
    ASSERT_EQ(symlink("/x-cbm-nowhere/stamp-pending", marker), 0);
    ASSERT_FALSE(cbm_file_exists(marker));
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    cbm_pipeline_test_fail_stamp(true);
    bi_run_t failed = bi_index(repo, -1);
    cbm_pipeline_test_fail_stamp(false);
    bool db_left = cbm_file_exists(db);
    (void)unlink(marker);

    ASSERT_NEQ(failed.rc, 0);
    ASSERT_FALSE(db_left);
    bi_cleanup(repo);
    PASS();
}
#endif

/* An incremental run whose own stamp fails does not refresh an existing
 * artifact: the copy would carry the new graph under the old branch. */
TEST(branch_failed_stamp_skips_incremental_export) {
    char *repo = bi_make_repo("cbm_bi_stampart");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", cbm_pipeline_project_name(p));
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", cbm_pipeline_repo_path(p));
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_exists(repo));
    int64_t before = bi_artifact_mtime(repo);

    cbm_usleep(1100000);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    cbm_pipeline_test_fail_stamp(true);
    bi_run_t failed = bi_index(repo, -1);
    cbm_pipeline_test_fail_stamp(false);
    int64_t after = bi_artifact_mtime(repo);

    ASSERT_NEQ(failed.rc, 0);
    ASSERT_EQ(after, before);
    bi_cleanup(repo);
    PASS();
}
#endif

/* A parked slot is one compressed file, smaller than the database; restoring
 * it brings back exactly the graph that was parked, and leaves no scratch
 * file behind. */
TEST(branch_compressed_slot_round_trip) {
    char *repo = bi_make_repo("cbm_bi_zst");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char sig_main[CBM_SHA256_HEX_LEN + 1];
    char sig[CBM_SHA256_HEX_LEN + 1];
    char path[1400];
    char db[1024];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(strlen(bi_graph_sig(sig_main, sizeof(sig_main))), CBM_SHA256_HEX_LEN);
    int64_t live = bi_fsize(bi_db_path(db, sizeof(db)));

    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    {
        bi_run_t rr = bi_index(repo, -1);
        ASSERT_STR_EQ(rr.action, "seeded");
    }
    ASSERT_TRUE(cbm_file_exists(bi_slot_file("main", ".db.zst", path, sizeof(path))));
    int64_t slot = bi_fsize(path);
    ASSERT_GT(slot, 0);
    ASSERT_LT(slot, live / 4); /* an index database compresses well */
    ASSERT_FALSE(cbm_file_exists(bi_slot_file("main", ".db", path, sizeof(path))));
    ASSERT_FALSE(cbm_file_exists(bi_slot_file("main", ".db.unz", path, sizeof(path))));
    ASSERT_FALSE(cbm_file_exists(bi_slot_file("main", ".db.zst.tmp", path, sizeof(path))));

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_STR_EQ(bi_graph_sig(sig, sizeof(sig)), sig_main);
    ASSERT_FALSE(cbm_file_exists(bi_slot_file("main", ".db.zst", path, sizeof(path))));
    ASSERT_FALSE(cbm_file_exists(bi_slot_file("main", ".db.unz", path, sizeof(path))));
    ASSERT_TRUE(cbm_file_exists(bi_slot_file("feature", ".db.zst", path, sizeof(path))));

    bi_cleanup(repo);
    PASS();
}

/* An uncompressed <slot>.db that an earlier build of this feature parked in
 * the root's directory restores as it is; the next park of that branch
 * writes a .db.zst and drops the .db. */
TEST(branch_legacy_uncompressed_slot_restores) {
    char *repo = bi_make_repo("cbm_bi_legacy_db");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char sig_main[CBM_SHA256_HEX_LEN + 1];
    char sig[CBM_SHA256_HEX_LEN + 1];
    char zst[1400];
    char legacy[1400];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    bi_graph_sig(sig_main, sizeof(sig_main));
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    bi_slot_file("main", ".db.zst", zst, sizeof(zst));
    bi_slot_file("main", ".db", legacy, sizeof(legacy));
    ASSERT_TRUE(cbm_branch_slot_decompress(zst, legacy));
    ASSERT_EQ(cbm_unlink(zst), 0);
    ASSERT_TRUE(bi_is_parked("main"));

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_EQ(r.reparsed, 0);
    ASSERT_STR_EQ(bi_graph_sig(sig, sizeof(sig)), sig_main);
    ASSERT_FALSE(cbm_file_exists(legacy));

    /* A legacy feature.db beside a fresh park of feature: superseded. */
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    {
        bi_run_t rr = bi_index(repo, -1);
        ASSERT_STR_EQ(rr.action, "restored");
    }
    bi_slot_file("feature", ".db", legacy, sizeof(legacy));
    bi_slot_file("main", ".db.zst", zst, sizeof(zst));
    ASSERT_TRUE(cbm_branch_slot_decompress(zst, legacy)); /* any database */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    {
        bi_run_t rr = bi_index(repo, -1);
        ASSERT_STR_EQ(rr.action, "restored");
    }
    ASSERT_FALSE(cbm_file_exists(legacy));
    ASSERT_TRUE(cbm_file_exists(bi_slot_file("feature", ".db.zst", zst, sizeof(zst))));
    char **list = NULL;
    int n = bi_parked(&list);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(list[0], "feature");
    cbm_branch_slots_free_list(list, n);

    bi_cleanup(repo);
    PASS();
}

/* A crash between compressing a park and removing the branch's legacy .db
 * leaves both. Restoring the .zst removes the stale .db as well: it must not
 * count against the cap, nor be restored by a later switch. */
TEST(branch_restore_removes_stale_legacy_twin) {
    char *repo = bi_make_repo("cbm_bi_twin");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char zst[1400];
    char legacy[1400];
    char side[1500];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked as .zst */
    bi_slot_file("main", ".db.zst", zst, sizeof(zst));
    bi_slot_file("main", ".db", legacy, sizeof(legacy));
    ASSERT_TRUE(cbm_branch_slot_decompress(zst, legacy)); /* the crash's leftover */
    snprintf(side, sizeof(side), "%s-wal", legacy);
    ASSERT_EQ(th_write_file(side, ""), 0);
    ASSERT_TRUE(cbm_file_exists(zst));

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    ASSERT_FALSE(cbm_file_exists(zst));
    ASSERT_FALSE(cbm_file_exists(legacy));
    ASSERT_FALSE(cbm_file_exists(side));
    ASSERT_FALSE(bi_is_parked("main"));

    bi_cleanup(repo);
    PASS();
}

/* A slot that does not decompress (truncated) is not restored: the outgoing
 * graph stays in place as the seed and the run reindexes the difference. */
TEST(branch_corrupt_slot_seeds) {
    char *repo = bi_make_repo("cbm_bi_corrupt");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char zst[1400];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    bi_slot_file("main", ".db.zst", zst, sizeof(zst));
    int64_t sz = bi_fsize(zst);
    ASSERT_GT(sz, 64);
#ifndef _WIN32
    ASSERT_EQ(truncate(zst, sz / 2), 0);
#else
    ASSERT_EQ(th_write_file(zst, "not a zstd frame"), 0);
#endif

    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("only_main"), 1);
    ASSERT_EQ(bi_count("only_feature"), 0);
    char path[1400];
    ASSERT_FALSE(cbm_file_exists(bi_slot_file("main", ".db.unz", path, sizeof(path))));
    /* The slot can never be restored: it is gone, not decompressed again on
     * the next switch to main, and the outgoing branch's park is kept. */
    ASSERT_FALSE(cbm_file_exists(zst));
    ASSERT_FALSE(bi_is_parked("main"));
    ASSERT_TRUE(bi_is_parked("feature"));

    /* A garbled slot found with no live database (the restore-into-nothing
     * path) goes too; the run indexes from scratch. */
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked again, feature restored */
    ASSERT_TRUE(bi_is_parked("main"));
    ASSERT_EQ(th_write_file(zst, "garbage, not a zstd frame"), 0);
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_EQ(cbm_unlink(db), 0);
    cbm_remove_db_sidecars(db);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_NEQ(r.action, "restored");
    ASSERT_EQ(bi_count("only_main"), 1);
    ASSERT_FALSE(cbm_file_exists(zst));

    bi_cleanup(repo);
    PASS();
}

/* The size budget evicts the least recently parked slots until the rest
 * fit, but never the one just parked. */
TEST(branch_budget_evicts_oldest) {
    char *repo = bi_make_repo("cbm_bi_budget");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_git(repo, "branch -q b1"), 0);
    ASSERT_EQ(bi_git(repo, "branch -q b2"), 0);

    /* No size cap: three slots. */
    ASSERT_EQ(bi_index_lim(repo, 8, 0).rc, 0); /* main */
    const char *order[] = {"b1", "b2", "feature"};
    for (int i = 0; i < 3; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "checkout -q %s", order[i]);
        ASSERT_EQ(bi_git(repo, cmd), 0);
        cbm_usleep(600000);
        cbm_usleep(600000);
        ASSERT_EQ(bi_index_lim(repo, 8, 0).rc, 0);
    }
    char **list = NULL;
    int n = bi_parked(&list);
    ASSERT_EQ(n, 3); /* main, b1, b2 */
    cbm_branch_slots_free_list(list, n);
    char path[1400];
    int64_t one = bi_fsize(bi_slot_file("b2", ".db.zst", path, sizeof(path)));
    ASSERT_GT(one, 0);

    /* Room for about two slots: parking feature drops main, then b1. */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    cbm_usleep(600000);
    cbm_usleep(600000);
    {
        bi_run_t rr = bi_index_lim(repo, 8, one * 2 + one / 2);
        ASSERT_STR_EQ(rr.action, "restored");
    }
    n = bi_parked(&list);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(list[0], "b2");
    ASSERT_STR_EQ(list[1], "feature");
    cbm_branch_slots_free_list(list, n);

    /* A budget below one slot keeps only the slot just parked. */
    ASSERT_EQ(bi_git(repo, "checkout -q b1"), 0);
    cbm_usleep(600000);
    cbm_usleep(600000);
    {
        bi_run_t rr = bi_index_lim(repo, 8, 1);
        ASSERT_STR_EQ(rr.action, "seeded");
    }
    n = bi_parked(&list);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(list[0], "main");
    cbm_branch_slots_free_list(list, n);

    bi_cleanup(repo);
    PASS();
}

/* Budget values: bytes with an optional binary suffix. */
TEST(branch_budget_parse) {
    int64_t v = -1;
    ASSERT_TRUE(cbm_branch_budget_parse("0", &v));
    ASSERT_EQ(v, 0);
    ASSERT_TRUE(cbm_branch_budget_parse("1048576", &v));
    ASSERT_EQ(v, 1048576);
    ASSERT_TRUE(cbm_branch_budget_parse("2G", &v));
    ASSERT_EQ(v, (int64_t)2 << 30);
    ASSERT_TRUE(cbm_branch_budget_parse("512MiB", &v));
    ASSERT_EQ(v, (int64_t)512 << 20);
    ASSERT_TRUE(cbm_branch_budget_parse("3gb", &v));
    ASSERT_EQ(v, (int64_t)3 << 30);
    ASSERT_TRUE(cbm_branch_budget_parse("1T", &v));
    ASSERT_EQ(v, (int64_t)1 << 40);
    v = 7;
    ASSERT_FALSE(cbm_branch_budget_parse("", &v));
    ASSERT_FALSE(cbm_branch_budget_parse("-1", &v));
    ASSERT_FALSE(cbm_branch_budget_parse("2X", &v));
    ASSERT_FALSE(cbm_branch_budget_parse("2Gi", &v));
    ASSERT_FALSE(cbm_branch_budget_parse("G", &v));
    ASSERT_FALSE(cbm_branch_budget_parse("99999999999T", &v));
    ASSERT_EQ(v, 7);
    PASS();
}

/* The budget reaches an index_repository run from the config key and from
 * CBM_BRANCH_INDEX_BUDGET (which wins). */
TEST(branch_budget_config_and_env) {
    char *repo = bi_make_repo("cbm_bi_budget_cfg");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_git(repo, "branch -q b1"), 0);

    cbm_config_t *cfg = cbm_config_open(cbm_resolve_cache_dir());
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_BRANCH_INDEX_BUDGET, "1"), 0);
    cbm_config_close(cfg);

    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0); /* main */
    snprintf(g_bi_root, sizeof(g_bi_root), "%s", repo);
    char *name = cbm_project_name_from_path(repo);
    snprintf(g_bi_project, sizeof(g_bi_project), "%s", name);
    free(name);
    ASSERT_EQ(bi_git(repo, "checkout -q b1"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0); /* main parked */
    cbm_usleep(600000);
    cbm_usleep(600000);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0); /* b1 parked, main evicted */
    char **list = NULL;
    int n = bi_parked(&list);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(list[0], "b1");
    cbm_branch_slots_free_list(list, n);

    /* The environment overrides the config: no size cap. */
    cbm_setenv("CBM_BRANCH_INDEX_BUDGET", "0", 1);
    cbm_usleep(600000);
    cbm_usleep(600000);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    ASSERT_EQ(cbm_mcp_index_in_process(repo), 0); /* feature parked, b1 kept */
    cbm_unsetenv("CBM_BRANCH_INDEX_BUDGET");
    n = bi_parked(&list);
    ASSERT_EQ(n, 2);
    cbm_branch_slots_free_list(list, n);

    cfg = cbm_config_open(cbm_resolve_cache_dir());
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_BRANCH_INDEX_BUDGET, ""), 0);
    cbm_config_close(cfg);
    bi_cleanup(repo);
    PASS();
}

/* Scratch files a crashed park or restore left beside the slots are swept
 * by the next switch; slots are not. */
TEST(branch_switch_sweeps_scratch) {
    char *repo = bi_make_repo("cbm_bi_sweep");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char unz[1400];
    char ztmp[1400];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(th_write_file(bi_slot_file("gone", ".db.unz", unz, sizeof(unz)), "x"), 0);
    ASSERT_EQ(th_write_file(bi_slot_file("gone", ".db.zst.tmp", ztmp, sizeof(ztmp)), "x"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    {
        bi_run_t rr = bi_index(repo, -1);
        ASSERT_STR_EQ(rr.action, "restored");
    }
    ASSERT_FALSE(cbm_file_exists(unz));
    ASSERT_FALSE(cbm_file_exists(ztmp));
    ASSERT_TRUE(bi_is_parked("feature"));

    bi_cleanup(repo);
    PASS();
}

/* A restore writes the whole database through the WAL. With another
 * connection holding the database open (a server's cached store), SQLite
 * cannot remove the WAL at close; the restore's own checkpoint and WAL reset
 * give the space back instead of leaving a database-sized -wal behind. */
TEST(branch_restore_shrinks_wal_under_open_reader) {
    char *repo = bi_make_repo("cbm_bi_walshrink");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char db[1024];
    char wal[1100];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);

    bi_db_path(db, sizeof(db));
    snprintf(wal, sizeof(wal), "%s-wal", db);
    cbm_store_t *reader = cbm_store_open_path_query(db);
    ASSERT_NOT_NULL(reader);
    ASSERT_EQ(bi_count("only_feature"), 1);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_STR_EQ(r.action, "restored");
    int64_t live = bi_fsize(db);
    int64_t w = bi_fsize(wal);
    ASSERT_GT(live, 0);
    ASSERT_LT(w, live / 2);
    cbm_store_close(reader);
    ASSERT_EQ(bi_count("only_main"), 1);

    bi_cleanup(repo);
    PASS();
}

/* On a case-insensitive filesystem "Feature" and "feature" have one slot
 * file: the park of the outgoing branch overwrites the incoming slot. The
 * restore must not then copy the outgoing graph back, report it restored and
 * delete the only park. Simulated on any POSIX filesystem: the incoming
 * slot is a symlink to the outgoing branch's slot file. */
TEST(branch_incoming_slot_same_file_as_outgoing) {
#ifndef _WIN32
    char *repo = bi_make_repo("cbm_bi_caseslot");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char main_zst[1400];
    char feat_zst[1400];
    char alias_zst[1400];

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked */
    bi_slot_file("main", ".db.zst", main_zst, sizeof(main_zst));
    bi_slot_file("feature", ".db.zst", feat_zst, sizeof(feat_zst));
    bi_slot_file("alias", ".db.zst", alias_zst, sizeof(alias_zst));
    /* A stale park of feature, and "alias" naming the same file. */
    ASSERT_EQ(link(main_zst, feat_zst), 0);
    ASSERT_EQ(symlink("feature.db.zst", alias_zst), 0);

    ASSERT_EQ(bi_git(repo, "checkout -q -b alias"), 0);
    bi_run_t r = bi_index(repo, -1);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("only_feature"), 1);
    ASSERT_TRUE(cbm_file_exists(feat_zst));
    ASSERT_TRUE(bi_is_parked("feature"));

    (void)cbm_unlink(alias_zst);
    bi_cleanup(repo);
#endif
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
typedef struct {
    char log[8192];
} bi_events_t;

/* Append "<event>:<basename of detail>;" to the log. */
static void bi_on_event(const char *event, const char *detail, void *data) {
    bi_events_t *ev = (bi_events_t *)data;
    const char *base = strrchr(detail, '/');
    base = base ? base + 1 : detail;
    size_t len = strlen(ev->log);
    snprintf(ev->log + len, sizeof(ev->log) - len, "%s:%s;", event, base);
}

/* Index of `needle` in log at or after `from`; -1 when absent. */
static int bi_event_at(const char *log, const char *needle, int from) {
    const char *hit = from >= 0 ? strstr(log + from, needle) : NULL;
    return hit ? (int)(hit - log) : -1;
}
#endif

/* A switch is crash-safe in order: the parked slot is on stable storage
 * (file and directory entry) before the restore overwrites the outgoing
 * graph, the restore commit is durable when it returns (synchronous FULL,
 * not NORMAL, whose WAL commits wait for a checkpoint), and only then is the
 * incoming slot removed. Restoring into a missing live database syncs the
 * installed file before the slot goes. */
TEST(branch_switch_syncs_before_unlink) {
#ifdef CBM_ENABLE_TEST_SEAMS
    char *repo = bi_make_repo("cbm_bi_sync");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    bi_events_t ev;

    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main parked */
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    ev.log[0] = '\0';
    cbm_branch_slots_test_on_event(bi_on_event, &ev);
    bi_run_t r = bi_index(repo, -1);
    cbm_branch_slots_test_on_event(NULL, NULL);
    ASSERT_STR_EQ(r.action, "restored");
    int at = bi_event_at(ev.log, "fsync:feature.db.zst.tmp;", 0);
    ASSERT_GTE(at, 0);
    at = bi_event_at(ev.log, "rename:feature.db.zst;", at);
    ASSERT_GTE(at, 0);
    at = bi_event_at(ev.log, "fsync_dir:", at);
    ASSERT_GTE(at, 0);
    at = bi_event_at(ev.log, "restore_commit:", at);
    ASSERT_GTE(at, 0);
    int level = atoi(ev.log + at + strlen("restore_commit:"));
    ASSERT_GTE(level, 2); /* FULL or EXTRA */
    ASSERT_GTE(bi_event_at(ev.log, "unlink_incoming:main.db.zst;", at), 0);

    /* No live database: the slot is decompressed and renamed into place. */
    char db[1024];
    bi_db_path(db, sizeof(db));
    ASSERT_EQ(cbm_unlink(db), 0);
    cbm_remove_db_sidecars(db);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    ev.log[0] = '\0';
    cbm_branch_slots_test_on_event(bi_on_event, &ev);
    r = bi_index(repo, -1);
    cbm_branch_slots_test_on_event(NULL, NULL);
    ASSERT_STR_EQ(r.action, "restored");
    char want[1200];
    const char *dbname = strrchr(db, '/') + 1;
    snprintf(want, sizeof(want), "fsync:%s;", "feature.db.unz.tmp");
    at = bi_event_at(ev.log, want, 0);
    ASSERT_GTE(at, 0);
    snprintf(want, sizeof(want), "rename:%s;", dbname);
    at = bi_event_at(ev.log, want, at);
    ASSERT_GTE(at, 0);
    at = bi_event_at(ev.log, "fsync_dir:", at);
    ASSERT_GTE(at, 0);
    ASSERT_GTE(bi_event_at(ev.log, "unlink_incoming:feature.db.zst;", at), 0);

    bi_cleanup(repo);
#endif
    PASS();
}

/* A branch whose name ends like a scratch file the module writes
 * ("fix.db.unz") has the slot fix.db.unz.db.zst: a slot, which the sweep of
 * scratch files at every switch must leave. */
TEST(branch_slot_named_like_scratch_survives_sweep) {
    char *repo = bi_make_repo("cbm_bi_scratchname");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char path[1400];

    ASSERT_EQ(bi_git(repo, "checkout -q -b fix.db.unz"), 0);
    ASSERT_EQ(bi_index(repo, -1).rc, 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    {
        bi_run_t rr = bi_index(repo, -1);
        ASSERT_STR_EQ(rr.action, "seeded");
    }
    ASSERT_TRUE(cbm_file_exists(bi_slot_file("fix.db.unz", ".db.zst", path, sizeof(path))));
    ASSERT_EQ(bi_git(repo, "checkout -q main"), 0);
    {
        bi_run_t rr = bi_index(repo, -1); /* sweeps, parks feature, seeds main */
        ASSERT_STR_EQ(rr.action, "seeded");
    }
    ASSERT_TRUE(cbm_file_exists(path));
    ASSERT_TRUE(bi_is_parked("fix.db.unz"));
    ASSERT_EQ(bi_git(repo, "checkout -q fix.db.unz"), 0);
    {
        bi_run_t rr = bi_index(repo, -1);
        ASSERT_STR_EQ(rr.action, "restored");
    }

    bi_cleanup(repo);
    PASS();
}

/* An index root given with a trailing separator still walks up to the
 * checkout's .git: "repo/src/" reads the same HEAD as "repo/src". */
TEST(branch_git_head_read_trailing_slash_root) {
    char *repo = bi_make_repo("cbm_bi_headslash");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    char slug[256];
    char p[600];

    snprintf(p, sizeof(p), "%s/", repo);
    ASSERT_STR_EQ(bi_head_slug(p, slug, sizeof(slug)), "main");
    snprintf(p, sizeof(p), "%s/src/", repo);
    ASSERT_STR_EQ(bi_head_slug(p, slug, sizeof(slug)), "main");
    snprintf(p, sizeof(p), "%s/src//", repo);
    ASSERT_STR_EQ(bi_head_slug(p, slug, sizeof(slug)), "main");

    th_rmtree(repo);
    PASS();
}

/* After a switch, a stored file the current mode does not discover that
 * exists but cannot be read is kept, as a file whose stat fails is: its
 * bytes are unknown, not different, and this mode would never re-extract
 * it once purged. */
TEST(branch_switch_keeps_unreadable_mode_skipped_file) {
#ifndef _WIN32
    if (geteuid() == 0) {
        PASS(); /* root reads a mode-000 file */
    }
    char *repo = bi_make_repo("cbm_bi_modeskip_unread");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    th_write_file(TH_PATH(repo, "docs/x.py"), "def doc_main():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m docs"), 0);

    ASSERT_EQ(bi_index_mode(repo, CBM_MODE_FULL).rc, 0);
    ASSERT_EQ(bi_count("doc_main"), 1);
    ASSERT_EQ(bi_git(repo, "checkout -q -b same-tree"), 0);
    char doc[700];
    snprintf(doc, sizeof(doc), "%s/docs/x.py", repo);
    ASSERT_EQ(chmod(doc, 0), 0);
    bi_run_t r = bi_index_mode(repo, CBM_MODE_FAST);
    (void)chmod(doc, 0644);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("doc_main"), 1);
    ASSERT_EQ(bi_count("only_main"), 1);

    bi_cleanup(repo);
#endif
    PASS();
}

/* A stored file the current mode does not discover whose stored digest is
 * missing or malformed (a row from before digests were stored) is kept on
 * a verifying switch: the bytes are unknown, not known to differ. */
TEST(branch_switch_keeps_mode_skipped_file_without_digest) {
    char *repo = bi_make_repo("cbm_bi_modeskip_nodigest");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    th_write_file(TH_PATH(repo, "docs/x.py"), "def doc_main():\n    return 1\n");
    ASSERT_EQ(bi_git(repo, "add ."), 0);
    ASSERT_EQ(bi_git(repo, "commit -q -m docs"), 0);

    ASSERT_EQ(bi_index_mode(repo, CBM_MODE_FULL).rc, 0);
    ASSERT_EQ(bi_count("doc_main"), 1);
    ASSERT_TRUE(bi_db_exec("UPDATE file_hashes SET sha256 = 'not-a-digest' "
                           "WHERE rel_path = 'docs/x.py';"));
    ASSERT_EQ(bi_git(repo, "checkout -q -b same-tree"), 0);
    bi_run_t r = bi_index_mode(repo, CBM_MODE_FAST);
    ASSERT_EQ(r.rc, 0);
    ASSERT_STR_EQ(r.action, "seeded");
    ASSERT_EQ(bi_count("doc_main"), 1);
    ASSERT_EQ(bi_count("only_main"), 1);

    bi_cleanup(repo);
    PASS();
}

/* ── Fixes that landed without a test of their own ──────────────── */

#ifdef CBM_ENABLE_TEST_SEAMS
static void bi_record_probe(const char *path, void *ud) {
    ((std::vector<std::string> *)ud)->push_back(path);
}
#endif

/* The direct HEAD reader's walk up from a root reaches the filesystem root
 * itself: "/.git" (a repository rooted at "/") and "C:\.git" (at a drive
 * root, the separator kept -- "C:" alone is drive-relative) are probed last. */
TEST(branch_git_head_read_probes_filesystem_root) {
#ifdef CBM_ENABLE_TEST_SEAMS
    std::vector<std::string> posix;
    std::vector<std::string> drive;
    cbm_git_head_t head;
    cbm_git_test_on_head_probe(bi_record_probe, &posix);
    (void)cbm_git_head_read("/cbm-no-such-top-dir/sub", &head);
    cbm_git_head_free(&head);
    cbm_git_test_on_head_probe(bi_record_probe, &drive);
    (void)cbm_git_head_read("C:\\cbm-no-such-top-dir\\sub", &head);
    cbm_git_head_free(&head);
    cbm_git_test_on_head_probe(NULL, NULL);
    ASSERT_EQ(posix.size(), 3);
    ASSERT_STR_EQ(posix[0].c_str(), "/cbm-no-such-top-dir/sub/.git");
    ASSERT_STR_EQ(posix[1].c_str(), "/cbm-no-such-top-dir/.git");
    ASSERT_STR_EQ(posix[2].c_str(), "/.git");
    ASSERT_EQ(drive.size(), 3);
    ASSERT_STR_EQ(drive[1].c_str(), "C:\\cbm-no-such-top-dir/.git");
    ASSERT_STR_EQ(drive[2].c_str(), "C:\\.git");
#else
    SKIP("requires CBM_TEST_SEAMS");
#endif
    PASS();
}

/* The submodule check's search for the superproject reaches the filesystem
 * root: a superproject rooted at "/" has its .git there. */
TEST(branch_gitlink_superproject_search_reaches_root) {
#if defined(CBM_ENABLE_TEST_SEAMS) && !defined(_WIN32)
    char *tmp = th_mktempdir("cbm_bi_superroot");
    ASSERT_NOT_NULL(tmp);
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/a/sub", tmp);
    ASSERT_TRUE(cbm_mkdir_p(sub, 0755));
    char link[600];
    snprintf(link, sizeof(link), "%s/.git", sub);
    ASSERT_EQ(th_write_file(link, "gitdir: ../../.git/modules/sub\n"), 0);
    std::vector<std::string> probes;
    cbm_git_test_on_head_probe(bi_record_probe, &probes);
    bool is_sub = cbm_git_gitlink_is_submodule(sub);
    cbm_git_test_on_head_probe(NULL, NULL);
    ASSERT_FALSE(is_sub); /* no enclosing repository at all */
    ASSERT_FALSE(probes.empty());
    ASSERT_STR_EQ(probes.back().c_str(), "/");
    th_rmtree(tmp);
#endif
    PASS();
}

#ifndef _WIN32
/* Copy the database at src to dst (a consistent snapshot). */
static bool bi_vacuum_into(const char *src, const char *dst) {
    sqlite3 *h = NULL;
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_open(src, &h) == SQLITE_OK &&
              sqlite3_prepare_v2(h, "VACUUM INTO ?1;", -1, &st, NULL) == SQLITE_OK &&
              sqlite3_bind_text(st, 1, dst, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(h);
    return ok;
}
#endif

/* A slot path that leaves less room in the path buffer than the longest
 * scratch suffix is refused (the switch stays in place) instead of letting
 * "<slot>.tmp" truncate back to the slot path itself; one with room parks.
 * The database path is padded with empty components ("dir////p.db"), which
 * SQLite folds and the slot directory keeps, to reach the limit without a
 * path SQLite cannot open. */
TEST(branch_slot_path_keeps_room_for_scratch_suffix) {
#ifndef _WIN32
    char *repo = bi_make_repo("cbm_bi_slotlen");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index(repo, -1).rc, 0); /* main */
    char src[1024];
    bi_db_path(src, sizeof(src));
    char key[CBM_BRANCH_ROOT_KEY_LEN + 1];
    ASSERT_TRUE(cbm_branch_root_key(g_bi_root, key, sizeof(key)));
    char name[CBM_BRANCH_SLOT_NAME_MAX + 1];
    ASSERT_TRUE(cbm_branch_slot_name("main", name, sizeof(name)));
    char *tmp = th_mktempdir("cbm_bi_slotlen_db");
    ASSERT_NOT_NULL(tmp);
    char base[256];
    snprintf(base, sizeof(base), "%s", tmp);
    std::string tail = std::string("/branches/") + g_bi_project + "/" + key + "/" + name + ".db.zst";
    /* The slot path's length: the longest that still parks, the shortest
     * refused, and one byte short of the buffer (where ".tmp" truncated
     * to the slot path itself). */
    const size_t lens[] = {CBM_PATH_MAX - 17, CBM_PATH_MAX - 16, CBM_PATH_MAX - 1};
    for (int i = 0; i < 3; i++) {
        char sub[300];
        snprintf(sub, sizeof(sub), "%s/c%d", base, i);
        ASSERT_TRUE(cbm_mkdir_p(sub, 0755));
        ASSERT_TRUE(strlen(sub) + tail.size() <= lens[i]);
        std::string dir = std::string(sub) + std::string(lens[i] - strlen(sub) - tail.size(), '/');
        std::string db = dir + "/p.db";
        std::string slot = dir + tail;
        ASSERT_EQ(slot.size(), lens[i]);
        ASSERT_TRUE(bi_vacuum_into(src, db.c_str()));
        char *from = NULL;
        cbm_branch_switch_t act =
            cbm_branch_slots_switch(db.c_str(), g_bi_project, "feature", g_bi_root, 8, 0, &from);
        free(from);
        if (i == 0) {
            ASSERT_EQ(act, CBM_BRANCH_SWITCH_SEEDED);
            ASSERT_TRUE(cbm_file_exists(slot.c_str()));
        } else {
            ASSERT_EQ(act, CBM_BRANCH_SWITCH_IN_PLACE);
            ASSERT_FALSE(cbm_file_exists(slot.c_str()));
        }
    }
    th_rmtree(base);
    bi_cleanup(repo);
#endif
    PASS();
}

/* Windows: a parked slot that refuses to go (held open without delete
 * sharing) is skipped by the eviction and not counted as gone: the next
 * oldest goes instead, and the cap is still met. */
TEST(branch_evict_skips_slot_that_stays) {
#ifdef _WIN32
    char *repo = bi_make_repo("cbm_bi_evheld");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;
    ASSERT_EQ(bi_index_lim(repo, 2, 0).rc, 0); /* main */
    char db[1024];
    char dir[1100];
    ASSERT_TRUE(cbm_branch_slots_dir(bi_db_path(db, sizeof(db)), g_bi_project, g_bi_root, dir,
                                     sizeof(dir)));
    ASSERT_TRUE(cbm_mkdir_p(dir, 0755));
    char a[1400];
    char b[1400];
    char c[1400];
    bi_slot_file("old-a", ".db.zst", a, sizeof(a));
    bi_slot_file("old-b", ".db.zst", b, sizeof(b));
    bi_slot_file("old-c", ".db.zst", c, sizeof(c));
    ASSERT_EQ(th_write_file(a, "a"), 0);
    ASSERT_EQ(th_write_file(b, "b"), 0);
    ASSERT_EQ(th_write_file(c, "c"), 0);
    ASSERT_EQ(bi_git(repo, "checkout -q feature"), 0);
    cbm_usleep(600000);
    cbm_usleep(600000);
    FILE *held = fopen(a, "rb"); /* no FILE_SHARE_DELETE: the unlink is refused */
    ASSERT_NOT_NULL(held);
    bi_run_t rr = bi_index_lim(repo, 2, 0); /* parks main: four slots, cap two */
    fclose(held);
    ASSERT_EQ(rr.rc, 0);
    ASSERT_TRUE(cbm_file_exists(a));
    ASSERT_FALSE(cbm_file_exists(b));
    ASSERT_FALSE(cbm_file_exists(c)); /* the held slot did not count as gone */
    ASSERT_TRUE(bi_is_parked("main"));
    char **list = NULL;
    int n = bi_parked(&list);
    cbm_branch_slots_free_list(list, n);
    ASSERT_EQ(n, 2);
    bi_cleanup(repo);
#endif
    PASS();
}

SUITE(branch_index) {
    char probe[64];
    snprintf(probe, sizeof(probe), "git --version >%s 2>&1", bi_null_dev());
    if (system(probe) != 0) {
        printf("  (git unavailable — branch_index suite skipped)\n");
        return;
    }
    RUN_TEST(branch_git_config_env_append);
    RUN_TEST(branch_slug_from_qn);
    RUN_TEST(branch_slugs_never_collide);
    RUN_TEST(branch_git_head_slug_reads_head_file);
    RUN_TEST(branch_git_head_read_non_ascii_path);
    RUN_TEST(branch_git_head_read_unc_without_git_ends);
#ifdef _WIN32
    RUN_TEST(branch_git_child_env_has_longpaths);
#endif
    RUN_TEST(branch_git_head_read_caps_line_length);
    RUN_TEST(branch_switch_parks_and_restores);
    RUN_TEST(branch_restore_keeps_recreated_empty_file);
    RUN_TEST(branch_switch_detached_shares_one_slot);
    RUN_TEST(branch_parked_hidden_status_and_delete);
    RUN_TEST(branch_park_cap_evicts_oldest);
    RUN_TEST(branch_index_disabled_reindexes_in_place);
    RUN_TEST(branch_switch_keeps_newest_adr);
    RUN_TEST(branch_seeded_artifact_names_new_branch);
    RUN_TEST(branch_index_config_applies_in_process);
    RUN_TEST(branch_identity_is_name_and_head);
    RUN_TEST(branch_head_read_matches_git);
    RUN_TEST(branch_name_containing_marker);
    RUN_TEST(branch_identity_from_older_databases);
    RUN_TEST(branch_subdir_root_sees_offline_switch);
    RUN_TEST(branch_missing_live_db_restores_parked);
    RUN_TEST(branch_reset_clears_orphaned_slots);
    RUN_TEST(branch_restore_is_visible_to_open_readers);
    RUN_TEST(branch_switch_uses_checkout_at_run_time);
#ifndef _WIN32
    RUN_TEST(branch_switch_rehashes_same_size_same_mtime);
#endif
    RUN_TEST(branch_stamp_keeps_fts_in_step);
    RUN_TEST(branch_stamp_repairs_torn_edge);
    RUN_TEST(branch_stamp_recreates_missing_edge);
    RUN_TEST(branch_unreadable_adr_blocks_restore);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(branch_switch_recomputes_session_brief);
    RUN_TEST(branch_restore_keeps_concurrent_adr_write);
#endif
    RUN_TEST(branch_stamp_only_switch_reexports_artifact);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(artifact_export_run_skips_rehash);
#endif
    RUN_TEST(branch_committed_artifact_stays_current);
    RUN_TEST(branch_artifact_stale_after_adr_update);
    RUN_TEST(branch_artifact_payload_mismatch_is_not_current);
    RUN_TEST(branch_artifact_adr_during_export_is_not_current);
    RUN_TEST(branch_dump_generation_is_sub_second);
    RUN_TEST(branch_restore_reexports_artifact);
    RUN_TEST(branch_index_prompt_counts_parked_only_cache);
    RUN_TEST(branch_long_slug_slot_names);
    RUN_TEST(branch_slot_name_avoids_windows_devices);
    RUN_TEST(branch_long_names_park_and_restore);
#ifndef _WIN32
    RUN_TEST(branch_requested_export_failure_fails_run);
#endif
    RUN_TEST(branch_slots_reject_other_roots);
    RUN_TEST(branch_delete_project_keeps_other_roots);
    RUN_TEST(branch_delete_by_path_keeps_unreadable_owner);
    RUN_TEST(branch_root_normalize_and_key);
    RUN_TEST(branch_legacy_slot_adopted);
    RUN_TEST(branch_legacy_slot_link_not_adopted);
    RUN_TEST(branch_switch_purges_changed_mode_skipped_file);
    RUN_TEST(incremental_purges_stale_artifact_dir_rows);
    RUN_TEST(branch_foreign_live_db_same_identity_rebuilds);
#ifndef _WIN32
    RUN_TEST(branch_artifact_behind_after_count_preserving_edit);
    RUN_TEST(branch_artifact_behind_after_format_rebuild);
    RUN_TEST(branch_slots_kept_when_live_db_undeletable);
    RUN_TEST(branch_artifact_bootstrap_adopted_by_importer);
#endif
    RUN_TEST(branch_session_pending_legacy_once);
    RUN_TEST(branch_name_shared_with_tag_matches_head_file);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(branch_session_start_notes_moved_checkout);
#endif
    RUN_TEST(branch_compressed_slot_round_trip);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(branch_failed_stamp_fails_run_and_blocks_park);
#ifndef _WIN32
    RUN_TEST(branch_failed_stamp_without_marker_removes_db);
#endif
    RUN_TEST(branch_failed_stamp_skips_incremental_export);
#endif
    RUN_TEST(branch_legacy_uncompressed_slot_restores);
    RUN_TEST(branch_restore_removes_stale_legacy_twin);
    RUN_TEST(branch_corrupt_slot_seeds);
    RUN_TEST(branch_budget_evicts_oldest);
    RUN_TEST(branch_budget_parse);
    RUN_TEST(branch_budget_config_and_env);
    RUN_TEST(branch_switch_sweeps_scratch);
    RUN_TEST(branch_restore_shrinks_wal_under_open_reader);
    RUN_TEST(branch_incoming_slot_same_file_as_outgoing);
    RUN_TEST(branch_switch_syncs_before_unlink);
    RUN_TEST(branch_slot_named_like_scratch_survives_sweep);
    RUN_TEST(branch_git_head_read_trailing_slash_root);
    RUN_TEST(branch_switch_keeps_unreadable_mode_skipped_file);
    RUN_TEST(branch_switch_keeps_mode_skipped_file_without_digest);
    RUN_TEST(branch_git_head_read_probes_filesystem_root);
    RUN_TEST(branch_gitlink_superproject_search_reaches_root);
    RUN_TEST(branch_slot_path_keeps_room_for_scratch_suffix);
    RUN_TEST(branch_evict_skips_slot_that_stays);
}
