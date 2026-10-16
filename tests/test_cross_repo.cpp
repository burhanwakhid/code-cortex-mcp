/*
 * test_cross_repo.cpp — Input validation, write-safety and matching guards for
 * the cross-repository matching pass. Ported from upstream
 * DeusData/codebase-memory-mcp tests/test_cross_repo.c (the parts that cover
 * #1133, #1459, #2133 and the removed 4,096-row scan cap).
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "pipeline/pass_cross_repo.h"
#include "store/store.h"

#include <sqlite3/sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char cache[256];
    char *saved_cache;
} cross_repo_fixture_t;

static bool cross_repo_fixture_begin(cross_repo_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *saved = getenv("CBM_CACHE_DIR");
    if (saved) {
        fixture->saved_cache = strdup(saved);
        if (!fixture->saved_cache) {
            return false;
        }
    }
    snprintf(fixture->cache, sizeof(fixture->cache), "/tmp/cbm-cross-repo-XXXXXX");
    return cbm_mkdtemp(fixture->cache) != NULL &&
           cbm_setenv("CBM_CACHE_DIR", fixture->cache, 1) == 0;
}

static void cross_repo_fixture_end(cross_repo_fixture_t *fixture) {
    if (fixture->saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", fixture->saved_cache, 1);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (fixture->cache[0]) {
        th_rmtree(fixture->cache);
    }
    free(fixture->saved_cache);
    memset(fixture, 0, sizeof(*fixture));
}

static bool cross_repo_project_path(const cross_repo_fixture_t *fixture, const char *project,
                                    char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s/%s.db", fixture->cache, project);
    return written > 0 && (size_t)written < out_size;
}

static bool cross_repo_create_project(const cross_repo_fixture_t *fixture, const char *project) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    bool ok = cbm_store_upsert_project(store, project, fixture->cache) == CBM_STORE_OK;
    cbm_store_close(store);
    return ok;
}

/* Seed one HTTP_CALLS/HANDLES pair into two project stores. The suffix keeps
 * node QNs unique when a source is linked to more than one target. */
static bool cross_repo_seed_http_pair(const cross_repo_fixture_t *fixture,
                                      const char *source_project, const char *target_project,
                                      const char *route_path, const char *suffix) {
    char source_path[512];
    char target_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path)) ||
        !cross_repo_project_path(fixture, target_project, target_path, sizeof(target_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }

    bool ok = cbm_store_upsert_project(source, source_project, fixture->cache) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, fixture->cache) == CBM_STORE_OK;
    char caller_qn[256];
    char local_route_qn[256];
    char target_route_qn[256];
    char handler_qn[256];
    char route_name[128];
    char edge_props[256];
    snprintf(caller_qn, sizeof(caller_qn), "%s.call.%s", source_project, suffix);
    snprintf(local_route_qn, sizeof(local_route_qn), "%s.local-route.%s", source_project, suffix);
    snprintf(target_route_qn, sizeof(target_route_qn), "__route__GET__%s", route_path);
    snprintf(handler_qn, sizeof(handler_qn), "%s.handle.%s", target_project, suffix);
    snprintf(route_name, sizeof(route_name), "GET %s", route_path);
    snprintf(edge_props, sizeof(edge_props), "{\"url_path\":\"%s\",\"method\":\"GET\"}",
             route_path);

    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "call_remote",
                         .qualified_name = caller_qn,
                         .file_path = "client.c"};
    cbm_node_t local_route = {.project = source_project,
                              .label = "Route",
                              .name = route_name,
                              .qualified_name = local_route_qn,
                              .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    int64_t local_route_id = ok ? cbm_store_upsert_node(source, &local_route) : 0;
    cbm_edge_t http_call = {.project = source_project,
                            .source_id = caller_id,
                            .target_id = local_route_id,
                            .type = "HTTP_CALLS",
                            .properties_json = edge_props};
    ok = ok && caller_id > 0 && local_route_id > 0 && cbm_store_insert_edge(source, &http_call) > 0;

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = route_name,
                               .qualified_name = target_route_qn,
                               .file_path = "server.c"};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "handle_remote",
                          .qualified_name = handler_qn,
                          .file_path = "server.c"};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;

    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

/* Give the source project its own handler for the local Route the seeded
 * HTTP_CALLS edge points at (caller-local HANDLES), as when a monorepo's
 * client code calls the API the same repo serves. (#1459) */
static bool cross_repo_seed_local_handler(const cross_repo_fixture_t *fixture,
                                          const char *source_project, const char *suffix) {
    char source_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    if (!source) {
        return false;
    }
    char local_route_qn[256];
    char handler_qn[256];
    snprintf(local_route_qn, sizeof(local_route_qn), "%s.local-route.%s", source_project, suffix);
    snprintf(handler_qn, sizeof(handler_qn), "%s.local-handle.%s", source_project, suffix);
    cbm_node_t route = {};
    bool ok =
        cbm_store_find_node_by_qn(source, source_project, local_route_qn, &route) == CBM_STORE_OK;
    int64_t route_id = ok ? route.id : 0;
    if (ok) {
        cbm_node_free_fields(&route);
    }
    cbm_node_t handler = {.project = source_project,
                          .label = "Function",
                          .name = "handle_local",
                          .qualified_name = handler_qn,
                          .file_path = "server.c"};
    int64_t handler_id = ok ? cbm_store_upsert_node(source, &handler) : 0;
    cbm_edge_t handles = {.project = source_project,
                          .source_id = handler_id,
                          .target_id = route_id,
                          .type = "HANDLES"};
    ok = ok && route_id > 0 && handler_id > 0 && cbm_store_insert_edge(source, &handles) > 0;
    cbm_store_close(source);
    return ok;
}

/* Give the source project a canonical "__route__GET__<path>" Route with its
 * own handler, distinct from the node the seeded HTTP_CALLS points at: the
 * shape extraction produces for a method-less fetch() against a local
 * app.get() route. (#1459) */
static bool cross_repo_seed_local_get_route(const cross_repo_fixture_t *fixture,
                                            const char *source_project, const char *route_path) {
    char source_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    if (!source) {
        return false;
    }
    char route_qn[256];
    char handler_qn[256];
    snprintf(route_qn, sizeof(route_qn), "__route__GET__%s", route_path);
    snprintf(handler_qn, sizeof(handler_qn), "%s.local-get-handler", source_project);
    cbm_node_t route = {.project = source_project,
                        .label = "Route",
                        .name = route_path,
                        .qualified_name = route_qn,
                        .file_path = "server.c"};
    cbm_node_t handler = {.project = source_project,
                          .label = "Function",
                          .name = "handle_local_get",
                          .qualified_name = handler_qn,
                          .file_path = "server.c"};
    int64_t route_id = cbm_store_upsert_node(source, &route);
    int64_t handler_id = cbm_store_upsert_node(source, &handler);
    cbm_edge_t handles = {.project = source_project,
                          .source_id = handler_id,
                          .target_id = route_id,
                          .type = "HANDLES"};
    bool ok = route_id > 0 && handler_id > 0 && cbm_store_insert_edge(source, &handles) > 0;
    cbm_store_close(source);
    return ok;
}

static bool cross_repo_exec(const cross_repo_fixture_t *fixture, const char *project,
                            const char *sql) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    char *error = NULL;
    int rc = sqlite3_exec(cbm_store_get_db(store), sql, NULL, NULL, &error);
    sqlite3_free(error);
    cbm_store_close(store);
    return rc == SQLITE_OK;
}

static int cross_repo_count_edges(const cross_repo_fixture_t *fixture, const char *project,
                                  const char *edge_type) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return -1;
    }
    cbm_store_t *store = cbm_store_open_path_query(path);
    if (!store) {
        return -1;
    }
    int count = cbm_store_count_edges_by_type(store, project, edge_type);
    cbm_store_close(store);
    return count;
}

/* A project store written before the #768 edges.local_name_gen column: it
 * still answers read-only queries (list_projects shows it), but the
 * read-write open every cross-repo target needs refuses it until a reindex. */
static bool cross_repo_create_pre768_project(const cross_repo_fixture_t *fixture,
                                             const char *project) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE projects(name TEXT PRIMARY KEY, indexed_at TEXT NOT NULL,"
             " root_path TEXT NOT NULL);"
             "CREATE TABLE nodes(id INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL,"
             " label TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL,"
             " file_path TEXT DEFAULT '', start_line INTEGER DEFAULT 0,"
             " end_line INTEGER DEFAULT 0, properties TEXT DEFAULT '{}',"
             " UNIQUE(project, qualified_name));"
             "CREATE TABLE edges(id INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL,"
             " source_id INTEGER NOT NULL, target_id INTEGER NOT NULL, type TEXT NOT NULL,"
             " properties TEXT DEFAULT '{}', UNIQUE(source_id, target_id, type));"
             "INSERT INTO projects VALUES('%s', '2026-06-01T00:00:00Z', '/pre768');",
             project);
    bool ok = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

/* #1133: a run whose targets resolve to nothing but the source project must
 * fail explicitly. It used to report "success" with projects_scanned:0, and
 * it had already wiped the source's existing CROSS_* edges. */
TEST(cross_repo_self_only_target_fails_and_keeps_edges_issue1133) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_http_pair(&fixture, "self-source", "self-target", "/self-only", "self");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed self-only fixture");
    }

    const char *target = "self-target";
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("self-source", &target, 1);
    int before = cross_repo_count_edges(&fixture, "self-source", "CROSS_HTTP_CALLS");
    const char *self = "self-source";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("self-source", &self, 1);
    int after = cross_repo_count_edges(&fixture, "self-source", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(initial.failed);
    ASSERT_TRUE(before > 0);
    ASSERT_TRUE(result.failed);
    ASSERT_TRUE(result.no_targets);
    ASSERT_EQ(result.projects_scanned, 0);
    ASSERT_EQ(after, before);
    PASS();
}

/* #1133: ["*"] in a cache that holds only the source project resolves to zero
 * targets: same contract as an explicit self-only list. */
TEST(cross_repo_wildcard_with_no_other_project_fails_issue1133) {
    cross_repo_fixture_t fixture;
    if (!cross_repo_fixture_begin(&fixture) ||
        !cross_repo_create_project(&fixture, "lonely-source")) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to create isolated source project");
    }

    const char *targets[] = {"*"};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("lonely-source", targets, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_TRUE(result.failed);
    ASSERT_TRUE(result.no_targets);
    ASSERT_EQ(result.projects_scanned, 0);
    PASS();
}

/* A named target that is not indexed fails before any write, and no ghost
 * .db is created for it. */
TEST(cross_repo_missing_named_target_fails_before_cleanup) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "miss-src", "miss-api", "/orders", "m");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed missing-target fixture");
    }
    const char *live[] = {"miss-api"};
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("miss-src", live, 1);
    int before = cross_repo_count_edges(&fixture, "miss-src", "CROSS_HTTP_CALLS");
    const char *with_missing[] = {"miss-api", "never-indexed"};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("miss-src", with_missing, 2);
    int after = cross_repo_count_edges(&fixture, "miss-src", "CROSS_HTTP_CALLS");
    char ghost[512];
    bool ghost_ok = cross_repo_project_path(&fixture, "never-indexed", ghost, sizeof(ghost));
    FILE *ghost_file = ghost_ok ? fopen(ghost, "rb") : NULL;
    bool ghost_created = ghost_file != NULL;
    if (ghost_file) {
        fclose(ghost_file);
    }
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(initial.failed);
    ASSERT_EQ(before, 1);
    ASSERT_TRUE(result.failed);
    ASSERT_FALSE(result.no_targets);
    ASSERT_EQ(after, before);
    ASSERT_FALSE(ghost_created);
    PASS();
}

/* #2133: a store the matcher's read-write open refuses (pre-#768 schema) is
 * not a wildcard target; it is skipped instead of failing the run. */
TEST(cross_repo_wildcard_skips_pre768_store_issue2133) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "wild-src", "wild-api", "/orders", "w") &&
                 cross_repo_create_pre768_project(&fixture, "aa-pre768-store");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed pre-#768 wildcard fixture");
    }

    const char *targets[] = {"*"};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("wild-src", targets, 1);
    int edges = cross_repo_count_edges(&fixture, "wild-src", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_EQ(result.http_edges, 1);
    ASSERT_EQ(edges, 1);
    PASS();
}

/* #2133: naming an unusable store stays an error, but it must be refused
 * during validation, before the source's previous CROSS_* generation is
 * deleted. */
TEST(cross_repo_named_pre768_target_fails_before_cleanup_issue2133) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "named-src", "named-api", "/orders", "n") &&
                 cross_repo_create_pre768_project(&fixture, "aa-pre768-store");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed pre-#768 named fixture");
    }

    const char *live[] = {"named-api"};
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("named-src", live, 1);
    int before = cross_repo_count_edges(&fixture, "named-src", "CROSS_HTTP_CALLS");
    const char *with_pre768[] = {"named-api", "aa-pre768-store"};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("named-src", with_pre768, 2);
    int after = cross_repo_count_edges(&fixture, "named-src", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(initial.failed);
    ASSERT_EQ(before, 1);
    ASSERT_TRUE(result.failed);
    ASSERT_EQ(after, before);
    PASS();
}

enum { CROSS_REPO_WIDE_CALLS = 4097 };

/* 4,097 HTTP_CALLS that all resolve to the target's one handled route. The
 * fork's loops stopped after 4,096 matches, so the last call never got its
 * CROSS_HTTP_CALLS edge even though the target serves its route. */
static bool cross_repo_seed_wide_scan(const cross_repo_fixture_t *fixture,
                                      const char *source_project, const char *target_project) {
    char source_path[512];
    char target_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path)) ||
        !cross_repo_project_path(fixture, target_project, target_path, sizeof(target_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }
    bool ok = cbm_store_upsert_project(source, source_project, fixture->cache) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, fixture->cache) == CBM_STORE_OK;
    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "wide_caller",
                         .qualified_name = "wide.source.caller",
                         .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    ok = ok && caller_id > 0 &&
         sqlite3_exec(cbm_store_get_db(source), "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    for (int i = 0; ok && i < CROSS_REPO_WIDE_CALLS; i++) {
        char name[64];
        char qn[96];
        snprintf(name, sizeof(name), "local_route_%d", i);
        snprintf(qn, sizeof(qn), "wide.source.route.%d", i);
        cbm_node_t local_route = {.project = source_project,
                                  .label = "Route",
                                  .name = name,
                                  .qualified_name = qn,
                                  .file_path = "client.c"};
        int64_t route_id = cbm_store_upsert_node(source, &local_route);
        cbm_edge_t edge = {.project = source_project,
                           .source_id = caller_id,
                           .target_id = route_id,
                           .type = "HTTP_CALLS",
                           .properties_json = "{\"url_path\":\"/wide\",\"method\":\"GET\"}"};
        ok = route_id > 0 && cbm_store_insert_edge(source, &edge) > 0;
    }
    if (ok) {
        ok = sqlite3_exec(cbm_store_get_db(source), "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    } else {
        (void)sqlite3_exec(cbm_store_get_db(source), "ROLLBACK", NULL, NULL, NULL);
    }

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = "GET /wide",
                               .qualified_name = "__route__GET__/wide",
                               .file_path = "server.c"};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "wide_handler",
                          .qualified_name = "wide.target.handler",
                          .file_path = "server.c"};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;
    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

/* Upstream 0ac529f5: no row count decides which calls are matched. */
TEST(cross_repo_matches_calls_past_4096_rows) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_wide_scan(&fixture, "wide-source", "wide-target");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed wide scan fixture");
    }
    const char *target = "wide-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("wide-source", &target, 1);
    int edges = cross_repo_count_edges(&fixture, "wide-source", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_EQ(result.http_edges, CROSS_REPO_WIDE_CALLS);
    ASSERT_EQ(edges, CROSS_REPO_WIDE_CALLS);
    PASS();
}

/* #1459 floor: a caller whose own project handles the route it calls is
 * calling itself; another project exposing the same path is not evidence of
 * a cross-service call. Neither direction of the pass may link it. */
TEST(cross_repo_caller_local_handler_blocks_cross_http_issue1459) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_http_pair(&fixture, "mono-1459", "api-1459", "/api/users", "mono") &&
        cross_repo_seed_local_handler(&fixture, "mono-1459", "mono");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed caller-local route fixture");
    }
    const char *target = "api-1459";
    cbm_cross_repo_result_t fwd = cbm_cross_repo_match("mono-1459", &target, 1);
    const char *consumer = "mono-1459";
    cbm_cross_repo_result_t rev = cbm_cross_repo_match("api-1459", &consumer, 1);
    int mono_edges = cross_repo_count_edges(&fixture, "mono-1459", "CROSS_HTTP_CALLS");
    int api_edges = cross_repo_count_edges(&fixture, "api-1459", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(fwd.failed);
    ASSERT_FALSE(rev.failed);
    ASSERT_EQ(fwd.http_edges, 0);
    ASSERT_EQ(rev.http_edges, 0);
    ASSERT_EQ(mono_edges, 0);
    ASSERT_EQ(api_edges, 0);
    PASS();
}

/* #1459 floor, real-world shape: a method-less client call points at the ANY
 * Route node, while the caller's own handler sits on the GET node for the same
 * path. The call is still served in-project and must not cross. */
TEST(cross_repo_methodless_call_to_local_get_route_blocks_cross_http_issue1459) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_http_pair(&fixture, "mono2-1459", "api3-1459", "/api/users", "mono2") &&
        cross_repo_exec(&fixture, "mono2-1459",
                        "UPDATE edges SET properties = '{\"url_path\":\"/api/users\"}' "
                        "WHERE type = 'HTTP_CALLS';") &&
        cross_repo_seed_local_get_route(&fixture, "mono2-1459", "/api/users");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed method-less caller-local fixture");
    }
    const char *target = "api3-1459";
    cbm_cross_repo_result_t fwd = cbm_cross_repo_match("mono2-1459", &target, 1);
    const char *consumer = "mono2-1459";
    cbm_cross_repo_result_t rev = cbm_cross_repo_match("api3-1459", &consumer, 1);
    int mono_edges = cross_repo_count_edges(&fixture, "mono2-1459", "CROSS_HTTP_CALLS");
    int api_edges = cross_repo_count_edges(&fixture, "api3-1459", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(fwd.failed);
    ASSERT_FALSE(rev.failed);
    ASSERT_EQ(fwd.http_edges, 0);
    ASSERT_EQ(rev.http_edges, 0);
    ASSERT_EQ(mono_edges, 0);
    ASSERT_EQ(api_edges, 0);
    PASS();
}

/* Control for the #1459 floor: without a caller-local handler the path match
 * still links the caller to the remote handler, in both run directions. */
TEST(cross_repo_caller_without_local_handler_keeps_cross_http_issue1459) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_http_pair(&fixture, "client-1459", "api2-1459", "/api/users", "client");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed remote-only route fixture");
    }
    const char *target = "api2-1459";
    cbm_cross_repo_result_t fwd = cbm_cross_repo_match("client-1459", &target, 1);
    int client_edges = cross_repo_count_edges(&fixture, "client-1459", "CROSS_HTTP_CALLS");
    int api_edges = cross_repo_count_edges(&fixture, "api2-1459", "CROSS_HTTP_CALLS");
    const char *consumer = "client-1459";
    cbm_cross_repo_result_t rev = cbm_cross_repo_match("api2-1459", &consumer, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(fwd.failed);
    ASSERT_FALSE(rev.failed);
    ASSERT_EQ(fwd.http_edges, 1);
    ASSERT_EQ(rev.http_edges, 1);
    ASSERT_EQ(client_edges, 1);
    ASSERT_EQ(api_edges, 1);
    PASS();
}

SUITE(cross_repo) {
    RUN_TEST(cross_repo_self_only_target_fails_and_keeps_edges_issue1133);
    RUN_TEST(cross_repo_wildcard_with_no_other_project_fails_issue1133);
    RUN_TEST(cross_repo_missing_named_target_fails_before_cleanup);
    RUN_TEST(cross_repo_wildcard_skips_pre768_store_issue2133);
    RUN_TEST(cross_repo_named_pre768_target_fails_before_cleanup_issue2133);
    RUN_TEST(cross_repo_matches_calls_past_4096_rows);
    RUN_TEST(cross_repo_caller_local_handler_blocks_cross_http_issue1459);
    RUN_TEST(cross_repo_methodless_call_to_local_get_route_blocks_cross_http_issue1459);
    RUN_TEST(cross_repo_caller_without_local_handler_keeps_cross_http_issue1459);
}
