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
#include <pipeline/artifact.h>
#include <pipeline/branch_slots.h>
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <foundation/platform.h>

#include <stdio.h>
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif

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
    ASSERT_STR_EQ(cbm_branch_slug_from_qn("proj.__branch__.main", "proj"), "main");
    /* A branch whose name contains the marker keeps all of it. */
    ASSERT_STR_EQ(cbm_branch_slug_from_qn("a.__branch__.b.__branch__.feature-x", "a"),
                  "b.__branch__.feature-x");
    ASSERT_NULL(cbm_branch_slug_from_qn("proj.main", "proj"));
    ASSERT_NULL(cbm_branch_slug_from_qn("proj.__branch__.", "proj"));
    ASSERT_NULL(cbm_branch_slug_from_qn("other.__branch__.main", "proj"));
    ASSERT_NULL(cbm_branch_slug_from_qn(NULL, "proj"));
    char dir[256];
    ASSERT_TRUE(cbm_branch_slots_dir("/c/proj.db", "proj", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "/c/branches/proj");
    ASSERT_FALSE(cbm_branch_slots_dir("/c/proj.db", "../x", dir, sizeof(dir)));
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
    ASSERT_STR_EQ(bi_head_slug(repo, slug, sizeof(slug)), "topic-x-y");
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
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);
    ASSERT_TRUE(cbm_artifact_exists(repo));
    /* The export added indexable files under .code-cortex/; take them into
     * main's graph so the switch below stays incremental. */
    ASSERT_EQ(bi_index(repo, -1).rc, 0);

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
static bool bi_head_matches_index(const char *repo) {
    cbm_branch_id_t current;
    cbm_branch_id_t indexed;
    if (!cbm_branch_id_from_head(repo, &current)) {
        return true;
    }
    char db[1024];
    cbm_store_t *s = cbm_store_open_path_query(bi_db_path(db, sizeof(db)));
    bool known = s && cbm_branch_id_indexed(s, g_bi_project, &indexed);
    cbm_store_close(s);
    return !known || cbm_branch_id_same(&indexed, &current);
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

    /* "a/b" and "a-b" share the slug "a-b" (and a slot), not an identity. */
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

    const char *files[] = {"live.db", "branches/live/main.db", "branches/orphan/main.db",
                           "branches/orphan/feature.db", "branches/busy/main.db"};
    for (int i = 0; i < 5; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", cache, files[i]);
        ASSERT_EQ(th_write_file(path, "x"), 0);
    }
    char busy[1024];
    snprintf(busy, sizeof(busy), "%s/busy.db", cache);
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(busy, &lease), 0);

    ASSERT_EQ(cbm_remove_indexes(NULL), 1); /* live.db */
    char path[1024];
    snprintf(path, sizeof(path), "%s/branches/live", cache);
    ASSERT_FALSE(cbm_file_exists(path));
    snprintf(path, sizeof(path), "%s/branches/orphan", cache);
    ASSERT_FALSE(cbm_file_exists(path));
    snprintf(path, sizeof(path), "%s/branches/busy/main.db", cache);
    ASSERT_TRUE(cbm_file_exists(path));

    cbm_db_lease_release(lease);
    ASSERT_EQ(cbm_remove_indexes(NULL), 0);
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
}
