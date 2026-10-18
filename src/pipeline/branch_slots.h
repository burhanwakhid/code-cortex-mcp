/*
 * branch_slots.h — One index per git branch, by parking the database.
 *
 * A checkout directory maps to exactly one project, and readers (tool
 * workers, the hook binary, list_projects) find its graph at
 * <cache_dir>/<project>.db. That name stays the graph of the branch that was
 * checked out at the last index run. When a run starts on another branch, the
 * outgoing branch's database is PARKED as
 *
 *     <cache_dir>/branches/<project>/<root key>/<slot>.db.zst
 *
 * (a zstd-compressed snapshot of the whole database, indexes included) and,
 * if the incoming branch was parked earlier, its content is decompressed
 * beside it and copied back
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
 * outside git. A name that is not its own slug carries a hash of the name
 * ("a/b" is "a-b-<hash>"), so distinct names keep distinct slots; names that
 * differ only in case still share one on a case-insensitive filesystem.
 * Whether a database matches the checkout is decided on the full identity the
 * Branch node's properties record (real branch name, detached, HEAD commit),
 * never on the slug, so a shared slot costs re-work, not a missed switch.
 *
 * Parked slots are capped per checkout by count and by total size; the least
 * recently parked go first. Earlier builds of this feature parked plain
 * <slot>.db files: those are still restored (and listed, counted, evicted and
 * deleted) as they are, and replaced by a .db.zst the next time that branch
 * is parked.
 *
 * The design and its trade-offs: docs/BRANCH_INDEX.md.
 *
 * Depends on: store, git (git_context), foundation (compat_fs, platform, log)
 */
#ifndef CBM_BRANCH_SLOTS_H
#define CBM_BRANCH_SLOTS_H

#include "git/git_context.h"
#include "store/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Parked databases kept per checkout; the least recently parked go first. */
    CBM_BRANCH_SLOTS_DEFAULT_KEEP = 8,
};

/* Default size budget of one checkout's parked slots, in bytes (2 GiB). At
 * the measured 4.5x-6.4x zstd ratios that keeps all 8 slots of a repository
 * whose index is up to about 1.2 GB, and one slot of LLVM's 7 GB index
 * (1.1 GB compressed). <= 0 means no size cap (the count cap still holds). */
#define CBM_BRANCH_SLOTS_DEFAULT_BUDGET ((int64_t)2 * 1024 * 1024 * 1024)

/* Parse a size budget: a non-negative integer of bytes with an optional
 * K, M, G or T suffix (powers of 1024; "B" or "iB" may follow, any case),
 * e.g. "2G", "512MiB", "1073741824". False (out untouched) when malformed. */
bool cbm_branch_budget_parse(const char *text, int64_t *out);

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
     * room for every suffix written beside it (".db.unz.tmp-journal"). */
    CBM_BRANCH_SLOT_NAME_MAX = 200,
};

/* The slot file stem for slug (cbm_branch_slots_list reports these): the slug
 * itself up to CBM_BRANCH_SLOT_NAME_MAX bytes, else a prefix of it plus '-'
 * and 16 hex digits of the SHA-256 of the whole slug. A slug that would name a
 * Windows device (CON, PRN, AUX, NUL, COM0-9, LPT0-9 before its first '.', any
 * case) gets '_' in front and the hash after, on every platform. bufsz must
 * exceed CBM_BRANCH_SLOT_NAME_MAX. */
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

enum { CBM_BRANCH_ROOT_KEY_LEN = 12 };

/* A short stable key for a checkout root: hex of the SHA-256 of its
 * canonical form (cbm_root_canonical), so every spelling of one directory
 * agrees. False when root is empty or bufsz <= CBM_BRANCH_ROOT_KEY_LEN. */
bool cbm_branch_root_key(const char *root, char *buf, size_t bufsz);

/* <dir of db_path>/branches/<project>/<root key>: one directory per checkout
 * root, because a derived project name can be several directories' and they
 * must never share slots. root NULL or "" gives the project directory
 * itself (every root's directories, and slots earlier builds parked there).
 * Returns false when it does not fit. */
bool cbm_branch_slots_dir(const char *db_path, const char *project, const char *root, char *buf,
                          size_t bufsz);

/* Park/restore before an index run on `slug`. The caller MUST hold the index
 * lease for db_path (cbm_pipeline_claim_index) — that lease is what excludes
 * other writers. Never leaves db_path missing or half-restored: parking only
 * reads db_path, and a restore is a single transaction on it.
 * `keep` bounds the parked count (< 1 disables parking: returns NONE).
 * `root` is the checkout being indexed: a live database or slot recorded for
 * another directory (a derived-name collision) is neither parked nor
 * restored, and a rejected slot stays in place.
 * `from` (optional) receives the slug the database was built for, allocated
 * (NULL when unknown); the caller frees it. */
cbm_branch_switch_t cbm_branch_slots_switch(const char *db_path, const char *project,
                                            const char *slug, const char *root, int keep,
                                            int64_t budget, char **from);

/* Decompress the parked slot file `slot` (a .db.zst) to the database file
 * out_path, through out_path.tmp and a rename, after removing out_path's
 * SQLite sidecars. False on a corrupt, truncated or unreadable slot. For
 * inspection and tests; a restore does this itself. */
bool cbm_branch_slot_decompress(const char *slot, const char *out_path);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Callback run when a restore starts, before it takes the live database's
 * write lock: a write made here must survive the restore. NULL clears. */
void cbm_branch_slots_test_before_restore(void (*callback)(void *), void *data);
/* Callback told of each durability step of a park or restore, in order:
 * "fsync" (a file's data synced; detail = its path), "fsync_dir" (a
 * directory synced), "rename" (detail = the destination), "restore_commit"
 * (detail = the restore connection's PRAGMA synchronous level, decimal) and
 * "unlink_incoming" (the restored slot removed). NULL clears. */
void cbm_branch_slots_test_on_event(void (*callback)(const char *event, const char *detail,
                                                     void *data),
                                    void *data);
#endif

/* Sorted slot stems (cbm_branch_slot_name) of root's parked databases; with
 * root NULL, every root's, as "<root key>/<stem>" (and bare stems for slots
 * an earlier build parked in the project directory). Returns the count (0
 * when none); *out is a malloc'd array of malloc'd strings, free with
 * cbm_branch_slots_free_list. */
int cbm_branch_slots_list(const char *db_path, const char *project, const char *root, char ***out);
void cbm_branch_slots_free_list(char **list, int count);

/* One parked slot file. */
typedef struct {
    char *name;      /* stem, as cbm_branch_slots_list reports it */
    char *file;      /* path relative to the project's slot directory */
    int64_t bytes;   /* size on disk */
    bool compressed; /* <slot>.db.zst; false for a legacy <slot>.db */
} cbm_branch_slot_info_t;

/* Every slot FILE of root's (root NULL: every root's) parked databases,
 * sorted by file; a branch can have two (both formats) after a crash.
 * Returns the count; free with cbm_branch_slots_free_info. */
int cbm_branch_slots_info(const char *db_path, const char *project, const char *root,
                          cbm_branch_slot_info_t **out);
void cbm_branch_slots_free_info(cbm_branch_slot_info_t *info, int count);

/* Delete every parked database of project -- every root's, and legacy ones
 * -- (plus sidecars and temp files) and the then-empty directories. Returns
 * the number of databases removed. */
int cbm_branch_slots_delete_all(const char *db_path, const char *project);

/* Whether the database at path was indexed from root (cbm_root_matches): a
 * derived project name can be another directory's, and so can its slots.
 * True when root is empty, and for a database that cannot be read or records
 * no project row or no root: nothing says it is another directory's. A
 * foreign database is logged (branch.foreign_db). */
bool cbm_branch_db_root_matches(const char *path, const char *project, const char *root);

/* Delete only the parked databases of project that belong to the checkout
 * at root (its root-key directory), then the directories left empty. Two
 * checkouts that derive the same project name keep separate slots; a prune
 * of one must not take the other's. Returns the number removed. */
int cbm_branch_slots_delete_root(const char *db_path, const char *project, const char *root);

/* Called by cbm_branch_slots_expire for each file it removes (or, in a dry
 * run, would remove): `path` the file, `bytes` its size, `scratch` true for a
 * scratch or temp file of an interrupted park or restore, false for a slot. */
typedef void (*cbm_branch_slot_expire_cb)(const char *path, int64_t bytes, bool scratch, void *ud);

/* Remove project's parked slots (every root's, and legacy ones) whose file
 * was last written -- parked -- before slot_before (epoch seconds; <= 0 skips
 * slots), and scratch files written before scratch_before (<= 0 skips them),
 * then the directories left empty. A restore deletes the slot it restores,
 * so a slot's mtime is the time it was parked and it has not been restored
 * since. The caller MUST hold the index lease of db_path. dry_run removes
 * nothing. Returns the number of slot files removed (or due). */
int cbm_branch_slots_expire(const char *db_path, const char *project, int64_t slot_before,
                            int64_t scratch_before, bool dry_run, cbm_branch_slot_expire_cb cb,
                            void *ud);

#ifdef __cplusplus
}
#endif

#endif /* CBM_BRANCH_SLOTS_H */
