/*
 * pass_cross_repo.h — Cross-repo intelligence: match Routes, Channels, and
 * async topics across indexed projects to create CROSS_* edges.
 */
#ifndef CBM_PASS_CROSS_REPO_H
#define CBM_PASS_CROSS_REPO_H

#include "store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of a cross-repo matching run. */
typedef struct {
    int http_edges;    /* CROSS_HTTP_CALLS edges created */
    int async_edges;   /* CROSS_ASYNC_CALLS edges created */
    int channel_edges; /* CROSS_CHANNEL edges created */
    int grpc_edges;    /* CROSS_GRPC_CALLS edges created */
    int graphql_edges; /* CROSS_GRAPHQL_CALLS edges created */
    int trpc_edges;    /* CROSS_TRPC_CALLS edges created */
    int projects_scanned;
    double elapsed_ms;
    bool failed;     /* source/target validation or the source open failed; no write happened */
    bool no_targets; /* failed: nothing but the source project resolved (upstream #1133) */
} cbm_cross_repo_result_t;

/* Run cross-repo matching for `project` against `target_projects`.
 * If target_count == 1 and target_projects[0] == "*", matches against all
 * indexed projects. Writes CROSS_* edges bidirectionally into both the
 * source and target project DBs.
 *
 * `project` and every named target must already be indexed. A named target
 * that is missing or unusable (pre-#768 schema), or a target list that
 * resolves to nothing but `project`, sets `failed` before any CROSS_* edge is
 * deleted; a "*" wildcard skips unusable stores.
 * Returns result with edge counts. */
cbm_cross_repo_result_t cbm_cross_repo_match(const char *project, const char **target_projects,
                                             int target_count);

#ifdef __cplusplus
}
#endif

#endif /* CBM_PASS_CROSS_REPO_H */
