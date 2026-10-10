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

/* A path for the wide file APIs. Past MAX_PATH an absolute drive path is
 * rewritten to the extended-length form (\\?\C:\...) the APIs accept
 * without the 260-character limit; git itself writes refs that long (a
 * branch name is unbounded). Shorter paths come back as cbm_utf8_to_wide
 * gives them, so this changes nothing for the ordinary case. */
static inline wchar_t *cbm_utf8_to_wide_path(const char *utf8) {
    wchar_t *w = cbm_utf8_to_wide(utf8);
    if (!w) {
        return nullptr;
    }
    size_t len = wcslen(w);
    bool is_drive = len >= 3 && ((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z')) &&
                    w[1] == L':' && (w[2] == L'\\' || w[2] == L'/');
    if (len < MAX_PATH || !is_drive) {
        return w;
    }
    wchar_t *ext = (wchar_t *)malloc((len + 5) * sizeof(wchar_t));
    if (!ext) {
        return w; /* the plain path: it fails as it would have */
    }
    wmemcpy(ext, L"\\\\?\\", 4);
    for (size_t i = 0; i <= len; i++) {
        ext[4 + i] = w[i] == L'/' ? L'\\' : w[i];
    }
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

#endif /* _WIN32 */
#endif /* CBM_WIN_UTF8_H */
