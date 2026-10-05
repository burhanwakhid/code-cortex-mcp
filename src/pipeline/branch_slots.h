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
 * and, if the incoming branch was parked earlier, its content is copied back
 * into <project>.db as one SQLite write transaction -- the file is never
 * swapped under a reader, and its WAL is never checkpointed or truncated
 * here. Otherwise the outgoing graph stays in place as the seed, so the
 * incremental pass pays only the diff between the branches either way. The
 * subdirectory keeps parked files out of every cache scan: those read only
 * top-level *.db entries.
 *
 * Parking is a cache, never a source of truth: the incremental pass decides
 * what to re-extract from the stored file hashes, not from the branch label,
 * so a wrong or stale slot costs time, not correctness. The one table that is
 * not derived from the source, the project's ADR, follows the checkout: a
 * restore installs the outgoing database's ADR, or its absence.
 *
 * The slug names the slot: a branch name made filesystem-safe (git_context.h),
 * "detached" for every detached HEAD (one shared slot), and "working-tree"
 * outside git. Two branch names that slug alike ("a/b", "a-b") share a slot.
 * Whether a database matches the checkout is decided on the full identity the
 * Branch node's properties record (real branch name, detached, HEAD commit),
 * never on the slug, so a shared slot costs re-work, not a missed switch.
 *
 * Depends on: store, git (git_context), foundation (compat_fs, platform, log)
 */
#ifndef CBM_BRANCH_SLOTS_H
#define CBM_BRANCH_SLOTS_H

#include "git/git_context.h"
#include "store/store.h"

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

/* The slug of project's Branch-node QN "<project>.__branch__.<slug>": the
 * text after that prefix, so a slug that itself contains the marker survives.
 * NULL when qn is not project's branch QN. Borrowed from qn. */
const char *cbm_branch_slug_from_qn(const char *qn, const char *project);

/* The git identity a database was built for, or a checkout has now. The
 * strings are whole (git bounds a branch name only by the filesystem), heap
 * allocated, and freed by cbm_branch_id_free. */
typedef struct {
    bool known; /* false: nothing to compare against */
    bool is_detached;
    bool has_sha; /* head_sha is recorded ("" then means an unborn branch) */
    char *branch; /* real branch name; NULL when detached or not recorded */
    char *slug;   /* filesystem-safe form of the name (cbm_git_branch_slug) */
    char head_sha[72];
} cbm_branch_id_t;

void cbm_branch_id_free(cbm_branch_id_t *id);

enum {
    /* Longest slot file name stem: one path component (NAME_MAX 255) less
     * room for every suffix written beside it (".db.tmp-journal"). */
    CBM_BRANCH_SLOT_NAME_MAX = 200,
};

/* The slot file stem for slug (cbm_branch_slots_list reports these): the slug
 * itself up to CBM_BRANCH_SLOT_NAME_MAX bytes, else a prefix of it plus '-'
 * and 16 hex digits of the SHA-256 of the whole slug. bufsz must exceed
 * CBM_BRANCH_SLOT_NAME_MAX. */
bool cbm_branch_slot_name(const char *slug, char *buf, size_t bufsz);

/* The identity recorded on project's Branch node: its properties, or for a
 * node without them only the slug from its QN. Returns out->known. */
bool cbm_branch_id_indexed(cbm_store_t *s, const char *project, cbm_branch_id_t *out);
/* The identity of a resolved checkout (subprocess-backed git_context). */
void cbm_branch_id_from_context(const cbm_git_context_t *ctx, cbm_branch_id_t *out);
/* The identity of the checkout at root from its HEAD files only (no git
 * subprocess). Returns out->known. */
bool cbm_branch_id_from_head(const char *root, cbm_branch_id_t *out);
/* True unless the two identities are known and differ: another branch name
 * (the slug when a name is not recorded), detached versus attached, or
 * another HEAD commit when both sides know theirs. */
bool cbm_branch_id_same(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current);
/* cbm_branch_id_same, except that an indexed identity with no recorded commit
 * does NOT match a checkout that has one: the graph's age is unknown. For
 * decisions that may cost a pass (content verification) or that report
 * (index_status); session start keeps the lenient form, which must stay quiet
 * where HEAD cannot be read from files. An unknown indexed identity never
 * matches a known checkout. */
bool cbm_branch_id_same_strict(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current);

/* <dir of db_path>/branches/<project>. Returns false when it does not fit. */
bool cbm_branch_slots_dir(const char *db_path, const char *project, char *buf, size_t bufsz);

/* Park/restore before an index run on `slug`. The caller MUST hold the index
 * lease for db_path (cbm_pipeline_claim_index) — that lease is what excludes
 * other writers. Never leaves db_path missing or half-restored: parking only
 * reads db_path, and a restore is a single transaction on it.
 * `keep` bounds the parked count (< 1 disables parking: returns NONE).
 * `from` (optional) receives the slug the database was built for, allocated
 * (NULL when unknown); the caller frees it. */
cbm_branch_switch_t cbm_branch_slots_switch(const char *db_path, const char *project,
                                            const char *slug, int keep, char **from);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Callback run when a restore starts, before it takes the live database's
 * write lock: a write made here must survive the restore. NULL clears. */
void cbm_branch_slots_test_before_restore(void (*callback)(void *), void *data);
#endif

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
