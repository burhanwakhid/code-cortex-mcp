/*
 * git_env.cpp — child environment for our own git spawns. See git_env.h.
 */
#include "foundation/git_env.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <crt_externs.h> /* _NSGetEnviron */
#else
extern char **environ;
#endif

const char *const cbm_git_repo_env_vars[CBM_GIT_REPO_ENV_VAR_COUNT] = {
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_CONFIG",
    "GIT_CONFIG_PARAMETERS",
    "GIT_CONFIG_COUNT",
    "GIT_OBJECT_DIRECTORY",
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_IMPLICIT_WORK_TREE",
    "GIT_GRAFT_FILE",
    "GIT_INDEX_FILE",
    "GIT_NO_REPLACE_OBJECTS",
    "GIT_REPLACE_REF_BASE",
    "GIT_PREFIX",
    "GIT_SHALLOW_FILE",
    "GIT_COMMON_DIR",
};

static char git_env_fold(char c) {
#ifdef _WIN32
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
#else
    return c;
#endif
}

/* Does the name part of `entry` (its first `name_len` chars) equal `var`? */
static bool git_env_name_equals(const char *entry, size_t name_len, const char *var) {
    size_t var_len = strlen(var);
    if (name_len != var_len) {
        return false;
    }
    for (size_t i = 0; i < var_len; i++) {
        if (git_env_fold(entry[i]) != var[i]) {
            return false;
        }
    }
    return true;
}

static bool git_env_name_is_repo_local(const char *name, size_t name_len) {
    for (size_t i = 0; i < CBM_GIT_REPO_ENV_VAR_COUNT; i++) {
        if (git_env_name_equals(name, name_len, cbm_git_repo_env_vars[i])) {
            return true;
        }
    }
    return false;
}

bool cbm_git_env_entry_is_repo_local(const char *entry) {
    if (!entry) {
        return false;
    }
    const char *eq = strchr(entry, '=');
    size_t name_len = eq ? (size_t)(eq - entry) : strlen(entry);
    return git_env_name_is_repo_local(entry, name_len);
}

#ifdef _WIN32

/* Wide entry name → is it repository-local? The names we match are ASCII, so
 * any non-ASCII code unit is an immediate mismatch. Windows keeps per-drive
 * cwd entries ("=C:=C:\\dir") whose name starts with '='; the separator search
 * starts at index 1 so those are kept untouched. */
static bool git_env_wide_entry_is_repo_local(const wchar_t *entry) {
    enum { GIT_ENV_NAME_CAP = 64 };
    char name[GIT_ENV_NAME_CAP] = {0};
    size_t n = 0;
    for (size_t i = 0; entry[i] && (i == 0 || entry[i] != L'='); i++) {
        if (entry[i] > 0x7F || n + 1 >= sizeof(name)) {
            return false;
        }
        name[n++] = (char)entry[i];
    }
    return git_env_name_is_repo_local(name, n);
}

/* GIT_CONFIG_KEY_<n> / GIT_CONFIG_VALUE_<n>: inert once GIT_CONFIG_COUNT is
 * scrubbed, and dropped so the block's own entries below cannot collide. */
static bool git_env_wide_entry_is_config_item(const wchar_t *entry) {
    return wcsncmp(entry, L"GIT_CONFIG_KEY_", 15) == 0 ||
           wcsncmp(entry, L"GIT_CONFIG_VALUE_", 17) == 0;
}

/* The one setting every git child gets: Git for Windows refuses a ref whose
 * loose file sits past MAX_PATH unless core.longpaths is on, and a branch
 * name is unbounded -- without it a long-named branch reads as detached. A
 * parent's GIT_CONFIG_COUNT is scrubbed (it may carry another repository's
 * settings), so this is the block's whole config set. */
static const wchar_t GIT_ENV_LONGPATHS[] =
    L"GIT_CONFIG_COUNT=1\0GIT_CONFIG_KEY_0=core.longpaths\0GIT_CONFIG_VALUE_0=true\0";

static bool git_env_wide_entry_dropped(const wchar_t *entry) {
    return git_env_wide_entry_is_repo_local(entry) || git_env_wide_entry_is_config_item(entry);
}

wchar_t *cbm_git_child_env_block(void) {
    wchar_t *current = GetEnvironmentStringsW();
    if (!current) {
        return NULL;
    }
    size_t kept = 0;
    for (const wchar_t *e = current; *e; e += wcslen(e) + 1) {
        if (!git_env_wide_entry_dropped(e)) {
            kept += wcslen(e) + 1;
        }
    }
    const size_t extra = sizeof(GIT_ENV_LONGPATHS) / sizeof(wchar_t) - 1; /* w/o final NUL */
    /* +2: a block that keeps nothing still needs its double terminator. */
    wchar_t *block = (wchar_t *)malloc((kept + extra + 2) * sizeof(wchar_t));
    if (!block) {
        FreeEnvironmentStringsW(current);
        return NULL;
    }
    size_t pos = 0;
    for (const wchar_t *e = current; *e; e += wcslen(e) + 1) {
        if (!git_env_wide_entry_dropped(e)) {
            size_t len = wcslen(e) + 1;
            memcpy(block + pos, e, len * sizeof(wchar_t));
            pos += len;
        }
    }
    memcpy(block + pos, GIT_ENV_LONGPATHS, extra * sizeof(wchar_t));
    pos += extra;
    block[pos] = L'\0';
    if (pos == 0) {
        block[1] = L'\0';
    }
    FreeEnvironmentStringsW(current);
    return block;
}

#else

char **cbm_git_child_envp(void) {
#ifdef __APPLE__
    char **env = *_NSGetEnviron();
#else
    char **env = environ;
#endif
    size_t count = 0;
    size_t bytes = 0;
    for (char **e = env; e && *e; e++) {
        if (!cbm_git_env_entry_is_repo_local(*e)) {
            count++;
            bytes += strlen(*e) + 1;
        }
    }
    size_t table = (count + 1) * sizeof(char *);
    char **envp = (char **)malloc(table + bytes);
    if (!envp) {
        return NULL;
    }
    char *strings = (char *)envp + table;
    size_t slot = 0;
    for (char **e = env; e && *e && slot < count; e++) {
        if (!cbm_git_env_entry_is_repo_local(*e)) {
            size_t len = strlen(*e) + 1;
            memcpy(strings, *e, len);
            envp[slot++] = strings;
            strings += len;
        }
    }
    envp[slot] = NULL;
    return envp;
}

#endif

void cbm_git_child_env_free(void *env) {
    free(env);
}
