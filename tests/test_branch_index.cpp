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
#include <git/git_context.h>
#include <mcp/mcp.h>
#include <pipeline/branch_slots.h>
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <foundation/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *bi_null_dev(void) {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

static int bi_git(const char *repo, const char *args) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" -c user.name=t -c user.email=t@t.io -c commit.gpgsign=false "
             "-c init.defaultBranch=main %s >%s 2>&1",
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
    return cbm_branch_slots_list(bi_db_path(db, sizeof(db)), g_bi_project, out);
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

TEST(branch_slug_from_qn) {
    ASSERT_STR_EQ(cbm_branch_slug_from_qn("proj.__branch__.main"), "main");
    ASSERT_STR_EQ(cbm_branch_slug_from_qn("a.__branch__.b.__branch__.feature-x"), "feature-x");
    ASSERT_NULL(cbm_branch_slug_from_qn("proj.main"));
    ASSERT_NULL(cbm_branch_slug_from_qn("proj.__branch__."));
    ASSERT_NULL(cbm_branch_slug_from_qn(NULL));
    char dir[256];
    ASSERT_TRUE(cbm_branch_slots_dir("/c/proj.db", "proj", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "/c/branches/proj");
    ASSERT_FALSE(cbm_branch_slots_dir("/c/proj.db", "../x", dir, sizeof(dir)));
    PASS();
}

/* The subprocess-free HEAD read agrees with cbm_git_context_resolve, also
 * through a linked worktree's .git file. */
TEST(branch_git_head_slug_reads_head_file) {
    char *repo = bi_make_repo("cbm_bi_head");
    ASSERT_NOT_NULL(repo);
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", repo);
    repo = saved;

    char *slug = cbm_git_head_slug(repo);
    ASSERT_NOT_NULL(slug);
    ASSERT_STR_EQ(slug, "main");
    free(slug);
    ASSERT_EQ(bi_git(repo, "checkout -q -b topic/x-y"), 0);
    slug = cbm_git_head_slug(repo);
    ASSERT_STR_EQ(slug, "topic-x-y");
    free(slug);
    ASSERT_EQ(bi_git(repo, "checkout -q --detach HEAD"), 0);
    slug = cbm_git_head_slug(repo);
    ASSERT_STR_EQ(slug, "detached");
    free(slug);

    char wt[512];
    snprintf(wt, sizeof(wt), "%s/wt", repo);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "worktree add -q -b side \"%s\"", wt);
    ASSERT_EQ(bi_git(repo, cmd), 0);
    slug = cbm_git_head_slug(wt);
    ASSERT_NOT_NULL(slug);
    ASSERT_STR_EQ(slug, "side");
    free(slug);
    cbm_git_context_t ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(wt, &ctx), 0);
    ASSERT_STR_EQ(ctx.branch_slug, "side");
    cbm_git_context_free(&ctx);

    char sub[600];
    snprintf(sub, sizeof(sub), "%s/src", repo);
    ASSERT_NULL(cbm_git_head_slug(sub)); /* not a checkout root */

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
    ASSERT_TRUE(cbm_branch_slots_dir(bi_db_path(db, sizeof(db)), g_bi_project, dir, sizeof(dir)));
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

SUITE(branch_index) {
    char probe[64];
    snprintf(probe, sizeof(probe), "git --version >%s 2>&1", bi_null_dev());
    if (system(probe) != 0) {
        printf("  (git unavailable — branch_index suite skipped)\n");
        return;
    }
    RUN_TEST(branch_slug_from_qn);
    RUN_TEST(branch_git_head_slug_reads_head_file);
    RUN_TEST(branch_switch_parks_and_restores);
    RUN_TEST(branch_switch_detached_shares_one_slot);
    RUN_TEST(branch_parked_hidden_status_and_delete);
    RUN_TEST(branch_park_cap_evicts_oldest);
    RUN_TEST(branch_index_disabled_reindexes_in_place);
}
