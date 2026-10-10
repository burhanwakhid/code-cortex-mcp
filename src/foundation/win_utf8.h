#ifndef CBM_WIN_UTF8_H
#define CBM_WIN_UTF8_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

static inline wchar_t *cbm_utf8_to_wide(const char *utf8) {
    if (!utf8) {
        return nullptr;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) {
        return nullptr;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, len);
    }
    return w;
}

/* Paths at least this long go to the wide file APIs in the extended-length
 * form. MAX_PATH bounds files; CreateDirectoryW stops 12 characters sooner
 * (room for an 8.3 name), so the directory limit sets the threshold. */
enum { CBM_WIN_LONG_PATH = MAX_PATH - 12 };

static inline bool cbm_wide_has_long_prefix(const wchar_t *w) {
    return w && w[0] == L'\\' && w[1] == L'\\' && w[2] == L'?' && w[3] == L'\\';
}

/* A path for the wide file APIs. Past CBM_WIN_LONG_PATH a path is made
 * absolute and normal (GetFullPathNameW, which has no length limit) and
 * rewritten to the extended-length form the APIs accept without the
 * 260-character limit: \\?\C:\... for a drive path, \\?\UNC\server\...
 * for a share. git writes refs that long (a branch name is unbounded), and a
 * branch slot named after one is too. Shorter paths, and any path that
 * cannot be resolved, come back as cbm_utf8_to_wide gives them, so this
 * changes nothing for the ordinary case. */
static inline wchar_t *cbm_utf8_to_wide_path(const char *utf8) {
    wchar_t *w = cbm_utf8_to_wide(utf8);
    if (!w || wcslen(w) < CBM_WIN_LONG_PATH || cbm_wide_has_long_prefix(w)) {
        return w;
    }
    DWORD need = GetFullPathNameW(w, 0, nullptr, nullptr);
    wchar_t *full = need ? (wchar_t *)malloc((size_t)need * sizeof(wchar_t)) : nullptr;
    DWORD got = full ? GetFullPathNameW(w, need, full, nullptr) : 0;
    if (got == 0 || got >= need) {
        free(full);
        return w; /* the plain path: it fails as it would have */
    }
    bool is_drive = got >= 3 &&
                    ((full[0] >= L'A' && full[0] <= L'Z') || (full[0] >= L'a' && full[0] <= L'z')) &&
                    full[1] == L':' && full[2] == L'\\';
    bool is_unc = got >= 3 && full[0] == L'\\' && full[1] == L'\\' && full[2] != L'?' &&
                  full[2] != L'.';
    if (!is_drive && !is_unc) {
        free(full);
        return w;
    }
    /* \\?\ + drive path, or \\?\UNC + the share path minus one leading
     * backslash. */
    const wchar_t *prefix = is_drive ? L"\\\\?\\" : L"\\\\?\\UNC";
    const wchar_t *rest = is_drive ? full : full + 1;
    size_t plen = wcslen(prefix);
    size_t rlen = wcslen(rest);
    wchar_t *ext = (wchar_t *)malloc((plen + rlen + 1) * sizeof(wchar_t));
    if (!ext) {
        free(full);
        return w;
    }
    wmemcpy(ext, prefix, plen);
    wmemcpy(ext + plen, rest, rlen + 1);
    free(full);
    free(w);
    return ext;
}

static inline char *cbm_wide_to_utf8(const wchar_t *wide) {
    if (!wide) {
        return nullptr;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return nullptr;
    }
    char *u8 = (char *)malloc((size_t)len);
    if (u8) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, u8, len, nullptr, nullptr);
    }
    return u8;
}

/* The UTF-8 extended-length form of a path past CBM_WIN_LONG_PATH, for an
 * API that takes UTF-8 and opens through the wide APIs without adding the
 * prefix itself (SQLite's win32 VFS accepts \\?\ paths as they are). NULL
 * when the path needs no rewrite or cannot take one: use it unchanged. */
static inline char *cbm_utf8_long_path(const char *utf8) {
    wchar_t *w = cbm_utf8_to_wide_path(utf8);
    char *out = cbm_wide_has_long_prefix(w) ? cbm_wide_to_utf8(w) : nullptr;
    free(w);
    return out;
}

#endif /* _WIN32 */
#endif /* CBM_WIN_UTF8_H */
