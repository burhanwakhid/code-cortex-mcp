/*
 * artifact.h — Persistent artifact export/import for team sharing.
 *
 * Exports the SQLite knowledge graph as a zstd-compressed artifact
 * to .code-cortex/graph.db.zst in the repository. Teammates
 * can import the artifact to bootstrap their local index instead
 * of running a full pipeline from scratch.
 */
#ifndef CBM_ARTIFACT_H
#define CBM_ARTIFACT_H

#include <stdbool.h>

/* Schema version — increment when DB schema changes (new tables/indexes).
 * Import refuses artifacts with schema_version > current.
 * v2: edges uniqueness widened to (source_id, target_id, type,
 *     local_name_gen) so sibling named imports coexist (#768) — old
 *     binaries cannot upsert against the widened constraint. */
#define CBM_ARTIFACT_SCHEMA_VERSION 2

#define CBM_ARTIFACT_FILENAME "graph.db.zst"
#define CBM_ARTIFACT_META "artifact.json"
#define CBM_ARTIFACT_DIR ".code-cortex"

#ifdef __cplusplus
extern "C" {
#endif

/* Export quality levels */
enum {
    CBM_ARTIFACT_FAST = 0, /* zstd -3, no index stripping (watcher path) */
    CBM_ARTIFACT_BEST = 1, /* zstd -9 + drop indexes + VACUUM INTO (explicit index) */
};

/* Export DB to .code-cortex/graph.db.zst artifact.
 * quality: CBM_ARTIFACT_FAST or CBM_ARTIFACT_BEST.
 * Creates .code-cortex/ dir, .gitattributes, and artifact.json.
 * Returns 0 on success, -1 on error. */
int cbm_artifact_export(const char *db_path, const char *repo_path, const char *project_name,
                        int quality);

/* Get details for the most recent export failure on this thread.
 * Returns NULL if no export error is recorded. */
const char *cbm_artifact_export_last_error(void);

/* Import artifact from .code-cortex/graph.db.zst to cache_db_path.
 * Decompresses, runs integrity check, recreates indexes, and records
 * repo_path as the project root (the importing checkout adopts the graph).
 * Returns 0 on success, -1 on error. */
int cbm_artifact_import(const char *repo_path, const char *cache_db_path);

/* Check if a compatible artifact exists in repo_path/.code-cortex/.
 * Returns true only if both graph.db.zst and artifact.json exist
 * and schema_version is compatible. */
bool cbm_artifact_exists(const char *repo_path);

/* Whether the artifact's metadata matches the live database at db_path: the
 * same branch (the Branch node's qualified name and branch name), the
 * same content fingerprint (cbm_store_content_fingerprint: the indexed
 * sources' digests, so any source edit moves it, count-preserving or not)
 * and the same graph generation (index format and the dump's indexed_at, so
 * a rebuild from unchanged sources after an upgrade moves it).
 * Reads artifact.json, one Branch query and one scan of file_hashes; never
 * decompresses the artifact. False when either side cannot be read or the
 * metadata predates these fields. The HEAD commit and the checkout's paths
 * are not compared, and artifact.json does not record them: committing the
 * artifact moves HEAD, and another developer's clone has other paths, and
 * neither changes the graph. */
bool cbm_artifact_is_current(const char *db_path, const char *repo_path, const char *project);

/* Get the git commit hash from artifact metadata. Caller must free().
 * Returns NULL if artifact doesn't exist or has no commit field. */
char *cbm_artifact_commit(const char *repo_path);

/* Whether repo_path is safe to interpolate into a double-quoted `git -C "…"` shell
 * command (as artifact.c does via cbm_popen). Rejects quote / backslash / shell
 * substitution metacharacters (cbm_validate_shell_arg); on Windows also rejects the
 * cmd.exe expansion metacharacters % ! ^. Spaces ARE allowed — double quotes handle
 * them on both POSIX sh and cmd.exe (single quotes, which cmd.exe does not honor,
 * were the pre-existing bug). Exposed so the shell-safety contract is unit-tested. */
bool cbm_artifact_repo_path_is_shell_safe(const char *repo_path);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Callback run by cbm_artifact_export after the payload is written and
 * before the metadata is read from the live database. NULL clears. */
void cbm_artifact_test_after_payload(void (*callback)(void *data), void *data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CBM_ARTIFACT_H */
