/*
 * git_context.h — Resolve git/worktree metadata for an indexed repo root.
 *
 * Captures whether a path is a git repo, whether it is a linked worktree,
 * the canonical (shared) repo root, the current branch (and a filesystem-safe
 * slug), HEAD/base SHAs, and a stable "branch root" qualified name used to
 * hang the structure graph off a Branch node.
 *
 * All git lookups go through cbm_popen with the repo path validated by
 * cbm_validate_shell_arg() before interpolation.
 *
 * Depends on: foundation (compat_fs, str_util, constants)
 */
#ifndef CBM_GIT_CONTEXT_H
#define CBM_GIT_CONTEXT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_git;
    bool is_worktree;
    bool is_detached;
    bool root_exists;
    char *input_path;
    char *worktree_root;
    char *git_dir;
    char *git_common_dir;
    char *canonical_root;
    char *branch;
    char *branch_slug;
    char *head_sha;
    char *base_sha;
} cbm_git_context_t;

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out);
void cbm_git_context_free(cbm_git_context_t *ctx);
char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx);
int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size);
/* The same JSON in an allocation sized to fit (a long branch name or path
 * would not fit a fixed buffer). NULL on failure. Caller frees. */
char *cbm_git_context_props_json_dup(const cbm_git_context_t *ctx);

/* What <dir>/.git says about dir, from one stat (no git subprocess): a main
 * checkout (.git is a directory), a gitlink (.git is a regular file: a linked
 * worktree or a submodule), or neither. Project resolvers that walk up from
 * cwd stop at a checkout root, so a nested worktree or repo is never answered
 * from the enclosing checkout's index. */
typedef enum {
    CBM_GIT_CHECKOUT_NONE = 0,
    CBM_GIT_CHECKOUT_MAIN,
    CBM_GIT_CHECKOUT_GITLINK,
} cbm_git_checkout_kind_t;

cbm_git_checkout_kind_t cbm_git_checkout_root_kind(const char *dir);

/* True when dir's .git is a gitlink naming "<common>/worktrees/<name>", i.e.
 * dir is a linked worktree rather than a submodule. Reads the small .git file. */
bool cbm_git_is_linked_worktree(const char *dir);
/* Whether dir's .git is a submodule's gitlink: its git dir is in a
 * superproject's "modules" directory. False for a linked worktree and for a
 * repository whose git dir was put elsewhere (git init --separate-git-dir),
 * which are checkouts of their own. */
bool cbm_git_gitlink_is_submodule(const char *dir);

/* What HEAD of the checkout containing `root` names, read from files only (no
 * git subprocess, for callers on a latency budget). root may be any directory
 * inside the checkout: the nearest enclosing `.git` (directory, or gitlink
 * file of a linked worktree or submodule) wins, as for git. Reads HEAD, the
 * gitdir's "commondir", the loose branch ref and packed-refs. branch is
 * the ref HEAD names with "refs/heads/" removed, as cbm_git_context_resolve
 * derives it (NULL when detached; free
 * with cbm_git_head_free);
 * head_sha is "" when the branch has no commit yet or its ref could not be
 * read from files. Returns non-zero when no enclosing checkout is found,
 * HEAD is unreadable, HEAD points outside refs/heads, or the repository keeps
 * its refs in a reftable. */
typedef struct {
    bool is_detached;
    char *branch; /* whole name, heap; NULL when detached */
    char head_sha[72];
} cbm_git_head_t;

int cbm_git_head_read(const char *root, cbm_git_head_t *out);
void cbm_git_head_free(cbm_git_head_t *head);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Callback told of every ".git" path cbm_git_head_read probes, in order, as
 * it walks up from the root. NULL clears. */
void cbm_git_test_on_head_probe(void (*callback)(const char *path, void *data), void *data);
#endif

/* The filesystem-safe slug of a branch name, as cbm_git_context_resolve
 * computes branch_slug ("detached" when detached, "working-tree" with no
 * name). A name that is not its own slug -- a character outside
 * [A-Za-z0-9._-], a leading '-', or the name "detached" or "working-tree" --
 * gets '-' and 12 hex digits of the SHA-256 of the name appended, so two
 * names never share a slug ("a/b" is "a-b-<hash>", "a-b" stays "a-b").
 * Caller frees. */
char *cbm_git_branch_slug(const char *branch, bool detached);

/* Append key=value to the configuration git reads from the environment
 * (GIT_CONFIG_COUNT, GIT_CONFIG_KEY_<n>, GIT_CONFIG_VALUE_<n>, git 2.31+),
 * after the entries already exported. A key already among them (compared
 * case-insensitively, as git does) is left as set. False when the exported
 * count is malformed: nothing changes. Not thread-safe (setenv). */
bool cbm_git_config_env_append(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* CBM_GIT_CONTEXT_H */
