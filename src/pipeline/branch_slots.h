/*
 * branch_slots.h — One index per git branch, by parking the database.
 *
 * A checkout directory maps to exactly one project, and readers (tool
 * workers, the hook binary, list_projects) find its graph at
 * <cache_dir>/<project>.db. That name stays the graph of the branch that was
 * checked out at the last index run. When a run starts on another branch, the
 * outgoing branch's database is PARKED as
 *
 *     <cache_dir>/branches/<project>/<slug>.db
 *
 * and, if the incoming branch was parked earlier, that database is moved back
 * into place. Otherwise the outgoing graph stays in place as the seed, so the
 * incremental pass pays only the diff between the branches either way. The
 * subdirectory keeps parked files out of every cache scan: those read only
 * top-level *.db entries.
 *
 * Parking is a cache, never a source of truth: the incremental pass decides
 * what to re-extract from the stored file hashes, not from the branch label,
 * so a wrong or stale slot costs time, not correctness.
 *
 * The slug is the suffix of the Branch node's QN (git_context.h): a branch
 * name made filesystem-safe, "detached" for every detached HEAD (one shared
 * slot), and "working-tree" outside git. Two branch names that slug alike
 * ("a/b", "a-b") share a slot.
 *
 * Depends on: store, foundation (compat_fs, platform, log)
 */
#ifndef CBM_BRANCH_SLOTS_H
#define CBM_BRANCH_SLOTS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Parked databases kept per project; the least recently parked go first. */
    CBM_BRANCH_SLOTS_DEFAULT_KEEP = 8,
};

typedef enum {
    CBM_BRANCH_SWITCH_NONE = 0, /* same branch, no stamp, or nothing to do */
    CBM_BRANCH_SWITCH_RESTORED, /* outgoing parked, incoming restored */
    CBM_BRANCH_SWITCH_SEEDED,   /* outgoing parked, kept in place as the seed */
    CBM_BRANCH_SWITCH_IN_PLACE, /* parking failed; reindex in place (pre-feature path) */
} cbm_branch_switch_t;

/* Stable lowercase name of a switch outcome ("none", "restored", ...). */
const char *cbm_branch_switch_str(cbm_branch_switch_t action);

/* The slug a Branch-node QN ends with: the text after the last
 * ".__branch__.". NULL when qn is not a branch QN. Borrowed from qn. */
const char *cbm_branch_slug_from_qn(const char *qn);

/* <dir of db_path>/branches/<project>. Returns false when it does not fit. */
bool cbm_branch_slots_dir(const char *db_path, const char *project, char *buf, size_t bufsz);

/* Park/restore before an index run on `slug`. The caller MUST hold the index
 * lease for db_path (cbm_pipeline_claim_index) — that lease is what excludes
 * other writers. Never leaves db_path missing: every step that could fail
 * runs before the single atomic rename that installs a restored database.
 * `keep` bounds the parked count (< 1 disables parking: returns NONE).
 * `from` (optional) receives the slug the database was built for. */
cbm_branch_switch_t cbm_branch_slots_switch(const char *db_path, const char *project,
                                            const char *slug, int keep, char *from, size_t from_sz);

/* Sorted slugs of the parked databases for project. Returns the count (0 when
 * none); *out is a malloc'd array of malloc'd strings, free with
 * cbm_branch_slots_free_list. */
int cbm_branch_slots_list(const char *db_path, const char *project, char ***out);
void cbm_branch_slots_free_list(char **list, int count);

/* Delete every parked database of project (plus sidecars and temp files) and
 * the then-empty directories. Returns the number of databases removed. */
int cbm_branch_slots_delete_all(const char *db_path, const char *project);

#ifdef __cplusplus
}
#endif

#endif /* CBM_BRANCH_SLOTS_H */
