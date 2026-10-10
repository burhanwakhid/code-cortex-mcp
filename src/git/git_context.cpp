#include "git/git_context.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h" /* cbm_is_dir — wide path on Windows */
#include "foundation/str_util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { GIT_CONFIG_ENV_MAX = 10000 };

/* Config keys compare case-insensitively (section and variable names). */
static bool git_config_key_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == *b;
}

bool cbm_git_config_env_append(const char *key, const char *value) {
    const char *count = getenv("GIT_CONFIG_COUNT");
    long n = 0;
    if (count && count[0]) {
        char *end = NULL;
        n = strtol(count, &end, 10);
        if (*end || n < 0 || n >= GIT_CONFIG_ENV_MAX) {
            return false; /* git rejects it too: leave the caller's value alone */
        }
    }
    char name[64];
    for (long i = 0; i < n; i++) {
        snprintf(name, sizeof(name), "GIT_CONFIG_KEY_%ld", i);
        const char *k = getenv(name);
        if (k && git_config_key_equal(k, key)) {
            return true; /* the caller's own setting wins */
        }
    }
    char num[24];
    snprintf(name, sizeof(name), "GIT_CONFIG_KEY_%ld", n);
    cbm_setenv(name, key, 1);
    snprintf(name, sizeof(name), "GIT_CONFIG_VALUE_%ld", n);
    cbm_setenv(name, value, 1);
    snprintf(num, sizeof(num), "%ld", n + 1);
    cbm_setenv("GIT_CONFIG_COUNT", num, 1);
    return true;
}

#ifdef _WIN32
/* Git for Windows refuses a ref whose loose file sits past MAX_PATH unless
 * core.longpaths is on, and a branch name is unbounded: without it the
 * identity git reports for such a checkout is "detached, no commit". Every
 * git this process runs gets the setting through the environment (git 2.31+
 * reads GIT_CONFIG_COUNT), appended to a caller's own GIT_CONFIG_* set. */
static bool git_longpaths_env_init(void) {
    (void)cbm_git_config_env_append("core.longpaths", "true");
    return true;
}
static const bool g_git_longpaths_env = git_longpaths_env_init();
#endif

enum {
    GIT_CMD_MAX = 1024,
    GIT_OUTPUT_MAX = 4096,
};

static char *git_strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

static void trim_newlines(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* One whole line from fp, newline stripped, however long (fgets into a fixed
 * buffer would cut a long branch name, and differently on each reader).
 * NULL at end of file. Caller frees. */
static char *read_line_dup(FILE *fp) {
    size_t cap = 128;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return NULL;
    }
    int c = EOF;
    while ((c = fgetc(fp)) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            char *grown = (char *)realloc(buf, cap * 2);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    trim_newlines(buf);
    return buf;
}

static bool git_validate_repo_path(const char *repo_path) {
    if (!cbm_validate_shell_arg(repo_path)) {
        return false;
    }
#ifdef _WIN32
    for (const char *p = repo_path; *p; p++) {
        if (*p == '%' || *p == '!' || *p == '^') {
            return false;
        }
    }
#endif
    return true;
}

static int git_capture(const char *repo_path, const char *git_args, char **out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;
    if (!repo_path || !git_args || !git_validate_repo_path(repo_path)) {
        return CBM_NOT_FOUND;
    }

    char cmd[GIT_CMD_MAX];
#ifdef _WIN32
    const char *null_dev = "NUL";
#else
    const char *null_dev = "/dev/null";
#endif
    /* Double quotes work for POSIX shells and cmd.exe. cbm_validate_shell_arg()
     * rejects quote/backslash/substitution metacharacters before interpolation. */
    int n = snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s 2>%s", repo_path, git_args, null_dev);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        return CBM_NOT_FOUND;
    }

    FILE *fp = cbm_popen_git(cmd);
    if (!fp) {
        return CBM_NOT_FOUND;
    }

    char *line = read_line_dup(fp);
    /* Drain: closing early would hand git SIGPIPE. */
    for (int ch = fgetc(fp); ch != EOF; ch = fgetc(fp)) {}
    int rc = cbm_pclose(fp);
    if (!line || rc != 0 || line[0] == '\0') {
        free(line);
        return CBM_NOT_FOUND;
    }
    *out = line;
    return 0;
}

/* Capture up to `max_lines` output lines instead of just the first.
 * Returns the number of non-empty lines captured (0 on any failure); the
 * caller owns each out[i]. Partial output on a nonzero exit is discarded, the
 * same all-or-nothing contract git_capture has.
 *
 * This exists so several `git rev-parse` questions can be asked in ONE
 * process. Each cbm_popen is a shell plus a git — ~16ms here — and resolving a
 * context asked seven of them, which was essentially all of index_status's
 * latency. */
static int git_capture_lines(const char *repo_path, const char *git_args, char **out,
                             int max_lines) {
    for (int i = 0; i < max_lines; i++) {
        out[i] = NULL;
    }
    if (!repo_path || !git_args || max_lines <= 0 || !git_validate_repo_path(repo_path)) {
        return 0;
    }

    char cmd[GIT_CMD_MAX];
#ifdef _WIN32
    const char *null_dev = "NUL";
#else
    const char *null_dev = "/dev/null";
#endif
    int n = snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s 2>%s", repo_path, git_args, null_dev);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        return 0;
    }

    FILE *fp = cbm_popen_git(cmd);
    if (!fp) {
        return 0;
    }
    char buf[GIT_OUTPUT_MAX];
    int count = 0;
    while (count < max_lines && fgets(buf, sizeof(buf), fp)) {
        trim_newlines(buf);
        if (!buf[0]) {
            break;
        }
        out[count] = git_strdup(buf);
        if (!out[count]) {
            break;
        }
        count++;
    }
    /* Drain: closing a pipe git is still writing to would hand it SIGPIPE and
     * turn a healthy run into a nonzero status. */
    while (fgets(buf, sizeof(buf), fp)) {
        ;
    }
    if (cbm_pclose(fp) != 0) {
        for (int i = 0; i < count; i++) {
            free(out[i]);
            out[i] = NULL;
        }
        return 0;
    }
    return count;
}

static bool path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/') {
        return true;
    }
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

static char *join_root_relative(const char *root, const char *rel) {
    if (!root || !root[0]) {
        return git_strdup(rel);
    }
    int n = snprintf(NULL, 0, "%s/%s", root, rel);
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    snprintf(out, (size_t)n + 1, "%s/%s", root, rel);
    return out;
}

/* Derive the canonical repo root.
 *
 * Preferred (git 2.31+): abs_common_dir is `git rev-parse --path-format=absolute
 * --git-common-dir` — git's OWN absolute, canonical common-dir. Because a main
 * repo and its linked worktree both ask the same git binary, they resolve to the
 * IDENTICAL path, so canonical_root is consistent across worktrees AND platforms
 * with no manual join or realpath/_fullpath — which diverged under msys vs native
 * path representations (#659 root cause, and the worktree==main-root breakage in
 * test_pipeline.c git_context_linked_worktree). git also resolves the relative
 * ".." internally, so the subdirectory case (#659) is fixed here too.
 *
 * Fallback (git < 2.31, no --path-format → abs_common_dir empty): the relative
 * --git-common-dir is relative to input_path (the -C dir), so join against it and
 * realpath-normalize the "..". Unix only — on Windows git emits an absolute
 * common-dir so this branch isn't reached in practice, and _fullpath there
 * reintroduces the msys divergence. */
static char *derive_canonical_root(const char *input_path, const char *worktree_root,
                                   const char *git_common_dir, const char *abs_common_dir) {
    char *root = NULL;
    if (abs_common_dir && abs_common_dir[0] && path_is_absolute(abs_common_dir)) {
        root = git_strdup(abs_common_dir);
        if (!root) {
            return NULL;
        }
    } else {
        const char *src = git_common_dir && git_common_dir[0] ? git_common_dir : worktree_root;
        if (!src) {
            return git_strdup("");
        }
#ifndef _WIN32
        root = path_is_absolute(src) ? git_strdup(src) : join_root_relative(input_path, src);
        if (!root) {
            return NULL;
        }
        {
            char resolved[4096];
            if (realpath(root, resolved) != NULL) {
                free(root);
                root = git_strdup(resolved);
                if (!root) {
                    return NULL;
                }
            }
        }
#else
        (void)input_path;
        root = path_is_absolute(src) ? git_strdup(src) : join_root_relative(worktree_root, src);
        if (!root) {
            return NULL;
        }
#endif
    }

    size_t len = strlen(root);
    while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) {
        root[--len] = '\0';
    }

    if (len >= 5 && strcmp(root + len - 5, "/.git") == 0) {
        root[len - 5] = '\0';
    }
#ifdef _WIN32
    else if (len >= 5 && strcmp(root + len - 5, "\\.git") == 0) {
        root[len - 5] = '\0';
    }
#endif

    return root;
}

static char *slug_from_branch(const char *branch, bool detached) {
    const char *fallback = detached ? "detached" : "working-tree";
    const char *src = detached ? fallback : (branch && branch[0] ? branch : fallback);
    size_t len = strlen(src);
    char *slug = (char *)malloc(len + 1);
    if (!slug) {
        return NULL;
    }

    size_t j = 0;
    bool in_dash = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            if (j == 0 && c == '-') {
                in_dash = true;
                continue;
            }
            slug[j++] = (char)c;
            in_dash = false;
        } else if (j > 0 && !in_dash) {
            slug[j++] = '-';
            in_dash = true;
        }
    }
    while (j > 0 && slug[j - 1] == '-') {
        j--;
    }
    slug[j] = '\0';

    if (slug[0] == '\0') {
        free(slug);
        return git_strdup(fallback);
    }
    return slug;
}

void cbm_git_context_free(cbm_git_context_t *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->input_path);
    free(ctx->worktree_root);
    free(ctx->git_dir);
    free(ctx->git_common_dir);
    free(ctx->canonical_root);
    free(ctx->branch);
    free(ctx->branch_slug);
    free(ctx->head_sha);
    free(ctx->base_sha);
    memset(ctx, 0, sizeof(*ctx));
}

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) {
        return CBM_NOT_FOUND;
    }

    out->input_path = git_strdup(path);
    if (!out->input_path) {
        return CBM_NOT_FOUND;
    }

    /* cbm_is_dir, not stat(): the Windows narrow stat() reads a UTF-8 path
     * through the ANSI code page, so an existing Unicode repository root looked
     * absent and git discovery stopped before git ran (upstream #1240). */
    out->root_exists = cbm_is_dir(path);
    if (!out->root_exists) {
        return 0;
    }

    /* One rev-parse answers every path question: it prints one line per query,
     * in argument order. `--path-format=absolute` applies to the options after
     * it, so the same --git-common-dir asked twice yields the repo-relative
     * form git reports by default and the absolute form (git 2.31+) used to
     * derive the canonical root.
     *
     * Older git does not know --path-format and fails the WHOLE command, so a
     * short read falls back to the three universally supported queries and
     * simply goes without the absolute form — which is what derive_canonical_
     * root's NULL branch has always handled. A repo with no commits still
     * answers all of these; only --verify HEAD below fails there, which is why
     * it stays a separate call. */
    enum { GIT_PATHS_FULL = 4, GIT_PATHS_MIN = 3 };
    char *paths[GIT_PATHS_FULL] = {NULL, NULL, NULL, NULL};
    int path_lines = git_capture_lines(path,
                                       "rev-parse --show-toplevel --git-dir --git-common-dir "
                                       "--path-format=absolute --git-common-dir",
                                       paths, GIT_PATHS_FULL);
    if (path_lines < GIT_PATHS_FULL) {
        for (int i = 0; i < path_lines; i++) {
            free(paths[i]);
            paths[i] = NULL;
        }
        path_lines = git_capture_lines(path, "rev-parse --show-toplevel --git-dir --git-common-dir",
                                       paths, GIT_PATHS_MIN);
    }
    if (path_lines < GIT_PATHS_MIN) {
        for (int i = 0; i < path_lines; i++) {
            free(paths[i]);
        }
        out->is_git = false;
        return 0;
    }
    out->is_git = true;
    out->worktree_root = paths[0];
    out->git_dir = paths[1];
    out->git_common_dir = paths[2];
    char *abs_common_dir = path_lines >= GIT_PATHS_FULL ? paths[3] : NULL;

    if (git_capture(path, "rev-parse --verify HEAD", &out->head_sha) != 0) {
        out->head_sha = git_strdup("");
    }

    if (git_capture(path, "symbolic-ref --quiet --short HEAD", &out->branch) != 0) {
        out->branch = git_strdup("DETACHED");
        out->is_detached = true;
    }

    out->is_worktree =
        out->git_dir && out->git_common_dir && strcmp(out->git_dir, out->git_common_dir) != 0;
    out->canonical_root =
        derive_canonical_root(path, out->worktree_root, out->git_common_dir, abs_common_dir);
    free(abs_common_dir);
    out->branch_slug = slug_from_branch(out->branch, out->is_detached);
    if (git_capture(path, "merge-base HEAD @{upstream}", &out->base_sha) != 0) {
        out->base_sha = git_strdup("");
    }

    if (!out->git_dir || !out->git_common_dir || !out->head_sha || !out->branch ||
        !out->canonical_root || !out->branch_slug || !out->base_sha) {
        cbm_git_context_free(out);
        return CBM_NOT_FOUND;
    }

    return 0;
}

/* First line of a small text file, newline stripped. False when unreadable. */
static bool read_first_line(const char *path, char *buf, size_t bufsz) {
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp) {
        return false;
    }
    bool ok = fgets(buf, (int)bufsz, fp) != NULL;
    (void)fclose(fp);
    if (ok) {
        trim_newlines(buf);
    }
    return ok;
}

/* The whole first line of a small text file; NULL when unreadable. */
static char *read_first_line_dup(const char *path) {
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    char *line = read_line_dup(fp);
    (void)fclose(fp);
    return line;
}

/* "<a><b><c>" in a new allocation. Caller frees. */
static char *str_cat3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t lc = strlen(c);
    char *out = (char *)malloc(la + lb + lc + 1);
    if (out) {
        memcpy(out, a, la);
        memcpy(out + la, b, lb);
        memcpy(out + la + lb, c, lc + 1);
    }
    return out;
}

/* "<dir>/<rel>", or rel itself when absolute. False when it does not fit. */
static bool path_under(const char *dir, const char *rel, char *buf, size_t bufsz) {
    int n = path_is_absolute(rel) ? snprintf(buf, bufsz, "%s", rel)
                                  : snprintf(buf, bufsz, "%s/%s", dir, rel);
    return n > 0 && (size_t)n < bufsz;
}

static bool is_object_id(const char *s) {
    size_t len = strlen(s);
    if (len != 40 && len != 64) { /* SHA-1, SHA-256 */
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* The commit refs/heads/<branch> names, from the loose ref file or
 * packed-refs in the common dir. False when neither has it: an unborn
 * branch, a symbolic ref, or a ref store this does not read. */
static bool read_branch_sha(const char *common, const char *branch, char *sha, size_t shasz) {
    char *ref = str_cat3("refs/heads/", branch, "");
    char *path = ref ? str_cat3(common, "/", ref) : NULL;
    char *line = path ? read_first_line_dup(path) : NULL;
    free(path);
    bool found = false;
    if (line) {
        found = is_object_id(line);
        if (found) {
            snprintf(sha, shasz, "%s", line);
        }
        free(line);
        free(ref);
        return found;
    }
    path = str_cat3(common, "/packed-refs", "");
    FILE *fp = path ? cbm_fopen(path, "rb") : NULL;
    free(path);
    while (fp && ref && !found && (line = read_line_dup(fp)) != NULL) {
        char *space = strchr(line, ' ');
        if (line[0] != '#' && line[0] != '^' && space) {
            *space = '\0';
            if (strcmp(space + 1, ref) == 0 && is_object_id(line)) {
                snprintf(sha, shasz, "%s", line);
                found = true;
            }
        }
        free(line);
    }
    if (fp) {
        (void)fclose(fp);
    }
    free(ref);
    return found;
}

int cbm_git_head_read(const char *root, cbm_git_head_t *out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    memset(out, 0, sizeof(*out));
    if (!root || !root[0]) {
        return CBM_NOT_FOUND;
    }
    char path[GIT_OUTPUT_MAX];
    char gitdir[GIT_OUTPUT_MAX];
    char line[GIT_OUTPUT_MAX];
    /* An index root can sit below the checkout root (repo/services/api): walk
     * up to the nearest directory holding a .git, as git itself does. */
    char top[GIT_OUTPUT_MAX];
    snprintf(top, sizeof(top), "%s", root);
    cbm_stat_t st; /* cbm_stat: non-ASCII and long checkout paths on Windows */
    for (;;) {
        int n = snprintf(path, sizeof(path), "%s/.git", top);
        if (n > 0 && (size_t)n < sizeof(path) && cbm_stat(path, &st) == 0) {
            break;
        }
        char *slash = strrchr(top, '/');
        char *bslash = strrchr(top, '\\');
        if (bslash && (!slash || bslash > slash)) {
            slash = bslash;
        }
        if (!slash || slash == top || slash[1] == '\0') {
            return CBM_NOT_FOUND; /* reached the filesystem root */
        }
        *slash = '\0';
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(gitdir, sizeof(gitdir), "%s", path);
    } else {
        /* Linked worktree or submodule: ".git" holds "gitdir: <dir>",
         * absolute or relative to the directory holding that file. */
        static const char prefix[] = "gitdir: ";
        if (!read_first_line(path, line, sizeof(line)) ||
            strncmp(line, prefix, sizeof(prefix) - 1) != 0 ||
            !path_under(top, line + sizeof(prefix) - 1, gitdir, sizeof(gitdir))) {
            return CBM_NOT_FOUND;
        }
    }
    /* Branch refs live in the common dir; a linked worktree's gitdir names it
     * in its "commondir" file, relative to the gitdir. */
    char common[GIT_OUTPUT_MAX];
    snprintf(path, sizeof(path), "%s/commondir", gitdir);
    if (!read_first_line(path, line, sizeof(line)) ||
        !path_under(gitdir, line, common, sizeof(common))) {
        snprintf(common, sizeof(common), "%s", gitdir);
    }
    snprintf(path, sizeof(path), "%s/HEAD", gitdir);
    char *head = read_first_line_dup(path); /* whole line: names are unbounded */
    if (!head) {
        return CBM_NOT_FOUND;
    }
    static const char ref_prefix[] = "ref: refs/heads/";
    int rc = CBM_NOT_FOUND;
    if (strncmp(head, ref_prefix, sizeof(ref_prefix) - 1) == 0) {
        const char *branch = head + sizeof(ref_prefix) - 1;
        /* A reftable repository keeps refs out of the file tree and points
         * HEAD at the placeholder "refs/heads/.invalid". */
        snprintf(path, sizeof(path), "%s/reftable", common);
        if (strcmp(branch, ".invalid") != 0 && !cbm_is_dir(path)) {
            out->branch = git_strdup(branch);
            (void)read_branch_sha(common, branch, out->head_sha, sizeof(out->head_sha));
            rc = out->branch ? 0 : CBM_NOT_FOUND;
        }
    } else if (is_object_id(head)) {
        /* A detached HEAD holds the commit id itself. */
        out->is_detached = true;
        snprintf(out->head_sha, sizeof(out->head_sha), "%s", head);
        rc = 0;
    }
    free(head);
    return rc;
}

void cbm_git_head_free(cbm_git_head_t *head) {
    if (head) {
        free(head->branch);
        head->branch = NULL;
    }
}

char *cbm_git_branch_slug(const char *branch, bool detached) {
    return slug_from_branch(branch, detached);
}

char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx) {
    const char *project = project_name && project_name[0] ? project_name : "project";
    const char *slug = "working-tree";
    if (ctx) {
        if (ctx->is_detached) {
            slug = "detached";
        } else if (ctx->is_git && ctx->branch_slug && ctx->branch_slug[0]) {
            slug = ctx->branch_slug;
        }
    }

    int n = snprintf(NULL, 0, "%s.__branch__.%s", project, slug);
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    snprintf(out, (size_t)n + 1, "%s.__branch__.%s", project, slug);
    return out;
}

static bool append_fmt_checked(char *buf, int buf_size, int *off, const char *fmt, ...) {
    if (!buf || !off || buf_size <= 0 || *off < 0 || *off >= buf_size) {
        return false;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, (size_t)(buf_size - *off), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= buf_size - *off) {
        buf[buf_size - 1] = '\0';
        return false;
    }
    *off += n;
    return true;
}

static int json_escaped_len(const char *src) {
    if (!src) {
        return 0;
    }
    int len = 0;
    for (int i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            len += 2;
        } else if (c < 0x20) {
            len += 6; /* \u00XX */
        } else {
            len++;
        }
    }
    return len;
}

static bool json_append_bool(char *buf, int buf_size, int *off, const char *name, bool value,
                             bool comma) {
    return append_fmt_checked(buf, buf_size, off, "\"%s\":%s%s", name, value ? "true" : "false",
                              comma ? "," : "");
}

static bool json_append_string(char *buf, int buf_size, int *off, const char *name,
                               const char *value, bool comma) {
    int needed = json_escaped_len(value ? value : "");
    char *escaped = (char *)malloc((size_t)needed + 1);
    if (!escaped) {
        return false;
    }
    int actual = cbm_json_escape(escaped, needed + 1, value ? value : "");
    bool ok = actual == needed && append_fmt_checked(buf, buf_size, off, "\"%s\":\"%s\"%s", name,
                                                     escaped, comma ? "," : "");
    free(escaped);
    return ok;
}

int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size) {
    if (!ctx || !buf || buf_size <= 0) {
        return 0;
    }

    int off = 0;
    bool ok =
        append_fmt_checked(buf, buf_size, &off, "{") &&
        json_append_bool(buf, buf_size, &off, "is_git", ctx->is_git, true) &&
        json_append_bool(buf, buf_size, &off, "is_worktree", ctx->is_worktree, true) &&
        json_append_bool(buf, buf_size, &off, "is_detached", ctx->is_detached, true) &&
        json_append_bool(buf, buf_size, &off, "root_exists", ctx->root_exists, true) &&
        json_append_string(buf, buf_size, &off, "canonical_root", ctx->canonical_root, true) &&
        json_append_string(buf, buf_size, &off, "worktree_root", ctx->worktree_root, true) &&
        json_append_string(buf, buf_size, &off, "git_common_dir", ctx->git_common_dir, true) &&
        json_append_string(buf, buf_size, &off, "branch", ctx->branch, true) &&
        json_append_string(buf, buf_size, &off, "head_sha", ctx->head_sha, true) &&
        json_append_string(buf, buf_size, &off, "base_sha", ctx->base_sha, false) &&
        append_fmt_checked(buf, buf_size, &off, "}");
    if (!ok) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    return off;
}

char *cbm_git_context_props_json_dup(const cbm_git_context_t *ctx) {
    for (int size = 4096; size <= (1 << 24); size *= 2) {
        char *buf = (char *)malloc((size_t)size);
        if (!buf) {
            return NULL;
        }
        if (cbm_git_context_props_json(ctx, buf, size) > 0) {
            return buf;
        }
        free(buf);
    }
    return NULL;
}

cbm_git_checkout_kind_t cbm_git_checkout_root_kind(const char *dir) {
    if (!dir || !dir[0]) {
        return CBM_GIT_CHECKOUT_NONE;
    }
    char dot_git[CBM_SZ_4K];
    if (snprintf(dot_git, sizeof(dot_git), "%s/.git", dir) >= (int)sizeof(dot_git)) {
        return CBM_GIT_CHECKOUT_NONE;
    }
    if (cbm_is_dir(dot_git)) {
        return CBM_GIT_CHECKOUT_MAIN;
    }
    return cbm_file_exists(dot_git) ? CBM_GIT_CHECKOUT_GITLINK : CBM_GIT_CHECKOUT_NONE;
}

bool cbm_git_is_linked_worktree(const char *dir) {
    if (cbm_git_checkout_root_kind(dir) != CBM_GIT_CHECKOUT_GITLINK) {
        return false;
    }
    char path[CBM_SZ_4K];
    snprintf(path, sizeof(path), "%s/.git", dir);
    FILE *f = cbm_fopen(path, "r");
    if (!f) {
        return false;
    }
    char line[CBM_SZ_4K];
    bool got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!got || strncmp(line, "gitdir:", 7) != 0) {
        return false;
    }
    /* "gitdir: <common>/worktrees/<name>" — a submodule's is ".../modules/<name>".
     * Strip the line end and trailing separators, then the <name> component. */
    char *val = line + 7;
    size_t n = strlen(val);
    for (size_t i = 0; i < n; i++) {
        if (val[i] == '\\') {
            val[i] = '/';
        }
    }
    while (n > 0 &&
           (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ' || val[n - 1] == '/')) {
        val[--n] = '\0';
    }
    char *name = strrchr(val, '/');
    if (!name) {
        return false;
    }
    *name = '\0';
    const char *parent = strrchr(val, '/');
    return parent && strcmp(parent + 1, "worktrees") == 0;
}
