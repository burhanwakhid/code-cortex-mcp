/*
 * branch_slots.cpp — Park and restore a project's database per git branch.
 * See branch_slots.h for the layout and the contract.
 */
#include "pipeline/branch_slots.h"

#include "store/store.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "foundation/str_util.h"

#include "sqlite3.h"
#include <yyjson/yyjson.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#endif

#define SLEN(s) (sizeof(s) - 1)

#include "zstd_store.h"

enum {
    BS_PATH_MAX = CBM_PATH_MAX,
    /* zstd level of a parked slot. Measured on two index databases (zstd
     * 1.5.7, one thread): level 3 gave 9.0x (redis, 216 MB) and 5.9x (opencv,
     * 462 MB) at about 600 MB/s; level 9 gave 10.0x and 6.7x at a fifth of
     * the speed. Parking sits on the branch-switch path, so the fast level. */
    BS_ZSTD_LEVEL = 3,
    BS_ZSTD_WORKERS = 4,
};

/* A decompressed slot is a database the index wrote; nothing near this has
 * been seen (the Linux kernel's index is ~14 GB). Bounds a corrupt frame. */
#define BS_MAX_UNPACKED_BYTES ((uint64_t)256 * 1024 * 1024 * 1024)

static const char BS_BRANCH_MARK[] = ".__branch__.";
/* Slot files: <name>.db.zst (compressed) and, from earlier builds of this
 * feature, <name>.db (a plain database). <name>.db.unz is the scratch copy a
 * park compresses from and a restore installs from. */
static const char BS_ZST_EXT[] = ".db.zst";
static const char BS_DB_EXT_S[] = ".db";
static const char BS_UNZ_EXT[] = ".db.unz";

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*on_event_cb)(const char *, const char *, void *) = nullptr;
static void *on_event_data = nullptr;

void cbm_branch_slots_test_on_event(void (*callback)(const char *, const char *, void *),
                                    void *data) {
    on_event_cb = callback;
    on_event_data = data;
}
#endif

/* Tell the test seam of a durability step (cbm_branch_slots_test_on_event). */
static void bs_event(const char *event, const char *detail) {
#ifdef CBM_ENABLE_TEST_SEAMS
    if (on_event_cb) {
        on_event_cb(event, detail, on_event_data);
    }
#else
    (void)event;
    (void)detail;
#endif
}

const char *cbm_branch_switch_str(cbm_branch_switch_t action) {
    switch (action) {
    case CBM_BRANCH_SWITCH_RESTORED:
        return "restored";
    case CBM_BRANCH_SWITCH_SEEDED:
        return "seeded";
    case CBM_BRANCH_SWITCH_IN_PLACE:
        return "in_place";
    case CBM_BRANCH_SWITCH_NONE:
    default:
        return "none";
    }
}

bool cbm_branch_budget_parse(const char *text, int64_t *out) {
    if (!text || !out) {
        return false;
    }
    const char *p = text;
    while (*p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (v > (UINT64_MAX - 9) / 10) {
            return false;
        }
        v = v * 10 + (uint64_t)(*p - '0');
    }
    int shift = 0;
    switch (*p) {
    case 'k':
    case 'K':
        shift = 10;
        break;
    case 'm':
    case 'M':
        shift = 20;
        break;
    case 'g':
    case 'G':
        shift = 30;
        break;
    case 't':
    case 'T':
        shift = 40;
        break;
    default:
        break;
    }
    if (shift) {
        p++;
        if (*p == 'i' || *p == 'I') {
            p++;
            if (*p != 'b' && *p != 'B') {
                return false;
            }
        }
    }
    if (*p == 'b' || *p == 'B') {
        p++;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p || v > ((uint64_t)INT64_MAX >> shift)) {
        return false;
    }
    *out = (int64_t)(v << shift);
    return true;
}

const char *cbm_branch_slug_from_qn(const char *qn, const char *project) {
    if (!qn || !project) {
        return NULL;
    }
    size_t plen = strlen(project);
    if (strncmp(qn, project, plen) != 0 ||
        strncmp(qn + plen, BS_BRANCH_MARK, SLEN(BS_BRANCH_MARK)) != 0 ||
        !qn[plen + SLEN(BS_BRANCH_MARK)]) {
        return NULL;
    }
    return qn + plen + SLEN(BS_BRANCH_MARK);
}

static const char *nz(const char *p) {
    return p ? p : "";
}

static void set_slug(cbm_branch_id_t *out) {
    free(out->slug);
    out->slug = cbm_git_branch_slug(out->branch, out->is_detached);
}

void cbm_branch_id_free(cbm_branch_id_t *id) {
    if (id) {
        free(id->branch);
        free(id->slug);
        id->branch = NULL;
        id->slug = NULL;
        id->known = false;
    }
}

bool cbm_branch_id_indexed(cbm_store_t *s, const char *project, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    char *qn = NULL;
    char *props = NULL;
    if (cbm_store_get_branch_dup(s, project, &qn, NULL, &props) != CBM_STORE_OK) {
        return false;
    }
    yyjson_doc *doc = props ? yyjson_read(props, strlen(props), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *is_git = root ? yyjson_obj_get(root, "is_git") : NULL;
    yyjson_val *branch = root ? yyjson_obj_get(root, "branch") : NULL;
    if (yyjson_get_bool(is_git) && yyjson_is_str(branch)) {
        out->is_detached = yyjson_get_bool(yyjson_obj_get(root, "is_detached"));
        if (!out->is_detached) {
            out->branch = strdup(yyjson_get_str(branch));
        }
        yyjson_val *sha = yyjson_obj_get(root, "head_sha");
        out->has_sha = yyjson_is_str(sha);
        snprintf(out->head_sha, sizeof(out->head_sha), "%s",
                 out->has_sha ? yyjson_get_str(sha) : "");
        set_slug(out);
    } else if (yyjson_is_bool(is_git)) {
        /* Indexed as a non-git tree (or git refused the checkout, e.g.
         * safe.directory): there is no branch to compare, and comparing
         * "working-tree" with the checkout's branch would mark every session
         * pending. Leave it unknown. */
    } else {
        /* No properties (they failed to serialize): the QN's slug is all
         * there is. */
        const char *slug = cbm_branch_slug_from_qn(qn, project);
        out->slug = slug ? strdup(slug) : NULL;
        out->is_detached = slug && strcmp(slug, "detached") == 0;
    }
    yyjson_doc_free(doc);
    free(qn);
    free(props);
    out->known = nz(out->slug)[0] != '\0';
    return out->known;
}

void cbm_branch_id_from_context(const cbm_git_context_t *ctx, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->is_git) {
        return;
    }
    out->is_detached = ctx->is_detached;
    if (!ctx->is_detached && ctx->branch) {
        out->branch = strdup(ctx->branch);
    }
    out->has_sha = ctx->head_sha != NULL;
    snprintf(out->head_sha, sizeof(out->head_sha), "%s", nz(ctx->head_sha));
    set_slug(out);
    out->known = nz(out->slug)[0] != '\0';
}

bool cbm_branch_id_from_head(const char *root, cbm_branch_id_t *out) {
    memset(out, 0, sizeof(*out));
    cbm_git_head_t head;
    if (cbm_git_head_read(root, &head) != 0) {
        return false;
    }
    out->is_detached = head.is_detached;
    out->branch = head.branch; /* ownership moves */
    /* An unreadable ref and an unborn branch both leave head_sha empty; only
     * a commit actually read is compared. */
    out->has_sha = head.head_sha[0] != '\0';
    snprintf(out->head_sha, sizeof(out->head_sha), "%s", head.head_sha);
    set_slug(out);
    out->known = nz(out->slug)[0] != '\0';
    return out->known;
}

bool cbm_branch_id_same_strict(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current) {
    if (!current || !current->known) {
        return true;
    }
    if (!indexed || !indexed->known) {
        return false;
    }
    bool indexed_sha = indexed->has_sha && indexed->head_sha[0] != '\0';
    return cbm_branch_id_same(indexed, current) && (indexed_sha || current->head_sha[0] == '\0');
}

bool cbm_branch_id_same(const cbm_branch_id_t *indexed, const cbm_branch_id_t *current) {
    if (!indexed || !current || !indexed->known || !current->known) {
        return true;
    }
    if (indexed->is_detached != current->is_detached) {
        return false;
    }
    if (!indexed->is_detached) {
        bool named = nz(indexed->branch)[0] && nz(current->branch)[0];
        if (named ? strcmp(indexed->branch, current->branch) != 0
                  : strcmp(nz(indexed->slug), nz(current->slug)) != 0) {
            return false;
        }
    }
    return !(indexed->has_sha && current->has_sha &&
             strcmp(indexed->head_sha, current->head_sha) != 0);
}

/* The slot file name for slug: the slug itself when it is short enough to be
 * one path component with room for every suffix this module and SQLite add
 * ("<name>.db.tmp-journal", "<name>.db-wal", ...). A longer slug -- a long or
 * deeply nested branch name, which git allows -- becomes a prefix of it plus
 * '-' and 16 hex digits of the SHA-256 of the whole slug, so distinct slugs
 * keep distinct slots and the same slug always maps to the same file. */
bool cbm_branch_slot_name(const char *slug, char *buf, size_t bufsz) {
    if (!slug || !buf || bufsz <= CBM_BRANCH_SLOT_NAME_MAX) {
        return false;
    }
    size_t len = strlen(slug);
    if (len <= CBM_BRANCH_SLOT_NAME_MAX) {
        snprintf(buf, bufsz, "%s", slug);
        return true;
    }
    enum { HASH_HEX = 16, PREFIX = CBM_BRANCH_SLOT_NAME_MAX - HASH_HEX - 1 };
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(slug, len, digest);
    snprintf(buf, bufsz, "%.*s-%.*s", (int)PREFIX, slug, (int)HASH_HEX, digest);
    return true;
}

/* A slug is one path component: the git_context slugger only emits
 * [A-Za-z0-9._-], but the stored QN is data, so check before building a path
 * from it. */
static bool slug_is_safe(const char *slug) {
    if (!slug || !slug[0] || strcmp(slug, ".") == 0 || strcmp(slug, "..") == 0) {
        return false;
    }
    for (const char *p = slug; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }
    return true;
}

bool cbm_branch_root_key(const char *root, char *buf, size_t bufsz) {
    char canon[CBM_ROOT_MAX];
    if (!buf || bufsz <= CBM_BRANCH_ROOT_KEY_LEN ||
        !cbm_root_canonical(root, canon, sizeof(canon))) {
        return false;
    }
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(canon, strlen(canon), digest);
    snprintf(buf, bufsz, "%.*s", (int)CBM_BRANCH_ROOT_KEY_LEN, digest);
    return true;
}

bool cbm_branch_slots_dir(const char *db_path, const char *project, const char *root, char *buf,
                          size_t bufsz) {
    if (!buf || bufsz == 0) {
        return false;
    }
    buf[0] = '\0';
    if (!db_path || !project || !cbm_validate_project_name(project)) {
        return false;
    }
    const char *slash = strrchr(db_path, '/');
    const char *bslash = strrchr(db_path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    int dir_len = slash ? (int)(slash - db_path) : 1;
    const char *dir = slash ? db_path : ".";
    char key[CBM_BRANCH_ROOT_KEY_LEN + 1] = "";
    if (root && root[0] && !cbm_branch_root_key(root, key, sizeof(key))) {
        return false;
    }
    int n =
        snprintf(buf, bufsz, "%.*s/branches/%s%s%s", dir_len, dir, project, key[0] ? "/" : "", key);
    if (n <= 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/* <dir>/<slot name of slug><ext>. */
static bool slot_file(const char *dir, const char *slug, const char *ext, char *buf, size_t bufsz) {
    char name[CBM_BRANCH_SLOT_NAME_MAX + 1];
    if (!cbm_branch_slot_name(slug, name, sizeof(name))) {
        return false;
    }
    int n = snprintf(buf, bufsz, "%s/%s%s", dir, name, ext);
    return n > 0 && (size_t)n < bufsz;
}

static bool ends_with(const char *name, const char *suffix) {
    size_t len = strlen(name);
    size_t slen = strlen(suffix);
    return len > slen && strcmp(name + len - slen, suffix) == 0;
}

/* The extension of a slot file name -- BS_ZST_EXT or BS_DB_EXT_S -- or NULL
 * for anything else in a slot directory (scratch, sidecars). */
static const char *slot_ext(const char *name) {
    if (ends_with(name, BS_ZST_EXT)) {
        return BS_ZST_EXT;
    }
    return ends_with(name, BS_DB_EXT_S) ? BS_DB_EXT_S : NULL;
}

/* Whether name ends with base followed by one of SQLite's sidecar suffixes
 * (or none, when bare is set). */
static bool ends_with_db_file(const char *name, const char *base, bool bare) {
    static const char *const sidecars[] = {"-wal", "-shm", "-journal"};
    char suffix[32];
    if (bare && ends_with(name, base)) {
        return true;
    }
    for (const char *side : sidecars) {
        snprintf(suffix, sizeof(suffix), "%s%s", base, side);
        if (ends_with(name, suffix)) {
            return true;
        }
    }
    return false;
}

/* The scratch copy a park compresses from and a restore installs from:
 * <name>.db.unz, its temp <name>.db.unz.tmp (dump and decompress), and their
 * SQLite sidecars. Matched by suffix only: a branch may be named
 * "fix.db.unz", and its slot fix.db.unz.db.zst is no scratch file. */
static bool is_unz_scratch(const char *name) {
    return ends_with_db_file(name, BS_UNZ_EXT, true) ||
           ends_with_db_file(name, ".db.unz.tmp", true);
}

/* Files this module writes beside the slots and removes again: the park and
 * restore scratch copy (is_unz_scratch), the compressor's <name>.db.zst.tmp,
 * the dump temp <name>.db.tmp of the uncompressed layout and its sidecars,
 * and the sidecars of a legacy <name>.db. */
static bool is_scratch_name(const char *name) {
    return is_unz_scratch(name) || ends_with(name, ".db.zst.tmp") ||
           ends_with_db_file(name, ".db.tmp", true) || ends_with_db_file(name, BS_DB_EXT_S, false);
}

/* The slug the database at db_path was built for, from its Branch node;
 * NULL when it records none. Caller frees. */
static char *read_indexed_slug(const char *db_path, const char *project) {
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return NULL;
    }
    cbm_branch_id_t id;
    bool ok = cbm_branch_id_indexed(s, project, &id);
    cbm_store_close(s);
    char *slug = NULL;
    if (ok && slug_is_safe(id.slug)) {
        slug = id.slug;
        id.slug = NULL;
    }
    cbm_branch_id_free(&id);
    return slug;
}

/* Copy the database at src to dest through the SQLite backup API: a read
 * transaction on src, so a consistent committed snapshot with its WAL frames
 * included, written to dest.tmp and renamed over dest after dest's own stale
 * sidecars are removed (cbm_store_dump_to_file, #897). src is only read --
 * no checkpoint, nothing done to its WAL, so readers of src are untouched.
 * dest must have no readers of its own: a slot, or a db_path that does not
 * exist. */
static bool copy_db(const char *src, const char *dest) {
    cbm_store_t *s = cbm_store_open_path_query(src);
    if (!s) {
        return false;
    }
    int rc = cbm_store_dump_to_file(s, dest);
    cbm_store_close(s);
    return rc == CBM_STORE_OK;
}

static int64_t file_mtime(const char *path) {
    cbm_stat_t st;
    if (cbm_stat(path, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_mtime;
}

static int64_t file_size(const char *path) {
    cbm_stat_t st;
    return cbm_stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static int64_t wal_size(const char *db_path) {
    char wal[BS_PATH_MAX];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    return file_size(wal);
}

/* Whether paths a and b name one existing file. POSIX compares device and
 * inode, which also catches two names a case-insensitive filesystem folds
 * together (and a symlink). Windows reports no inode number through
 * _wstat64; its filesystems compare names without case, so the names are
 * compared that way there. */
static bool same_file(const char *a, const char *b) {
    cbm_stat_t sa;
    cbm_stat_t sb;
    if (cbm_stat(a, &sa) != 0 || cbm_stat(b, &sb) != 0) {
        return false;
    }
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

static void log_kv(const char *msg, const char *k1, int64_t v1, const char *k2, int64_t v2,
                   const char *k3, int64_t v3) {
    char b1[32];
    char b2[32];
    char b3[32];
    snprintf(b1, sizeof(b1), "%lld", (long long)v1);
    snprintf(b2, sizeof(b2), "%lld", (long long)v2);
    snprintf(b3, sizeof(b3), "%lld", (long long)v3);
    cbm_log_info(msg, k1, b1, k2, b2, k3, b3);
}

/* True when the slot file is gone afterwards: an unlink refused (a read-only
 * cache, a Windows sharing violation) leaves it, and the caller must not
 * report it removed. */
static bool unlink_slot(const char *path) {
    int rc = cbm_unlink(path);
    cbm_remove_db_sidecars(path);
    return rc == 0 || !cbm_file_exists(path);
}

/* Remove a scratch database and everything SQLite or the dump put beside it. */
static void unlink_scratch(const char *path) {
    char tmp[BS_PATH_MAX];
    unlink_slot(path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink_slot(tmp);
    snprintf(tmp, sizeof(tmp), "%s-journal", path);
    (void)cbm_unlink(tmp);
}

/* Write f's data through to stable storage: its stdio buffer, then fsync
 * (_commit on Windows). path names f for the test seam. */
static bool sync_stream(FILE *f, const char *path) {
    if (fflush(f) != 0) {
        return false;
    }
#ifdef _WIN32
    bool ok = _commit(cbm_fileno(f)) == 0;
#else
    bool ok = fsync(cbm_fileno(f)) == 0;
#endif
    if (ok) {
        bs_event("fsync", path);
    }
    return ok;
}

/* Sync the file at path (an installed database). */
static bool sync_file(const char *path) {
    FILE *f = cbm_fopen(path, "r+b");
    if (!f) {
        return false;
    }
    bool ok = sync_stream(f, path);
    return fclose(f) == 0 && ok;
}

/* Sync the directory holding path, so a rename into it survives a power
 * loss. On POSIX an fsync of the directory; best effort, since a filesystem
 * that cannot sync a directory has nothing more to offer and the rename
 * cannot be taken back. Windows has nothing to do: NTFS journals its
 * metadata and cbm_rename_replace renames with write-through. */
static void sync_parent_dir(const char *path) {
#ifndef _WIN32
    char dir[BS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return;
    }
    *slash = '\0';
    int fd = open(dir[0] ? dir : "/", O_RDONLY);
    if (fd < 0) {
        return;
    }
    if (fsync(fd) == 0) {
        bs_event("fsync_dir", dir);
    }
    (void)close(fd);
#else
    bs_event("fsync_dir", path);
#endif
}

/* Compress the file at src into dst (one zstd frame with a checksum),
 * through dst.tmp and a rename, so dst is complete or absent. dst is on
 * stable storage when this returns true: a restore overwrites the outgoing
 * graph right after, and this slot is then its only copy. */
static bool compress_file(const char *src, const char *dst) {
    char tmp[BS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    FILE *in = cbm_fopen(src, "rb");
    FILE *out = in ? cbm_fopen(tmp, "wb") : NULL;
    int64_t n = out ? cbm_zstd_compress_stream(in, out, file_size(src), BS_ZSTD_LEVEL,
                                               BS_ZSTD_WORKERS)
                    : -1;
    bool ok = n > 0 && sync_stream(out, tmp);
    if (out && fclose(out) != 0) {
        ok = false;
    }
    if (in) {
        (void)fclose(in);
    }
    if (ok && cbm_rename_replace(tmp, dst) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)cbm_unlink(tmp);
    } else {
        bs_event("rename", dst);
        sync_parent_dir(dst);
    }
    return ok;
}

/* cbm_branch_slot_decompress; durable: out_path is on stable storage when
 * this returns true (a database about to replace the slot it came from). */
static bool decompress_file(const char *slot, const char *out_path, bool durable) {
    if (!slot || !out_path) {
        return false;
    }
    char tmp[BS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
    FILE *in = cbm_fopen(slot, "rb");
    FILE *out = in ? cbm_fopen(tmp, "wb") : NULL;
    int64_t n = out ? cbm_zstd_decompress_stream(in, out, BS_MAX_UNPACKED_BYTES) : -1;
    bool ok = n > 0 && (!durable || sync_stream(out, tmp));
    if (out && fclose(out) != 0) {
        ok = false;
    }
    if (in) {
        (void)fclose(in);
    }
    if (ok) {
        /* No SQLite sidecar of an earlier file at out_path may be replayed
         * onto this one (#897). */
        cbm_remove_db_sidecars(out_path);
        ok = cbm_rename_replace(tmp, out_path) == 0;
        if (ok) {
            bs_event("rename", out_path);
        }
    }
    if (!ok) {
        (void)cbm_unlink(tmp);
    }
    return ok;
}

bool cbm_branch_slot_decompress(const char *slot, const char *out_path) {
    return decompress_file(slot, out_path, false);
}

/* Park the live database at db_path as dir/<slot of slug>.db.zst: a
 * consistent snapshot (copy_db: one read transaction through the backup API,
 * which neither blocks readers nor needs a checkpoint) into the scratch file
 * <slot>.db.unz, compressed to <slot>.db.zst.tmp and renamed into place. A
 * legacy <slot>.db of the same branch is superseded and removed. *slot_bytes
 * receives the compressed size. */
static bool park_db(const char *db_path, const char *dir, const char *slug, char *zst,
                    size_t zst_sz, int64_t *slot_bytes) {
    char scratch[BS_PATH_MAX];
    char legacy[BS_PATH_MAX];
    if (!slot_file(dir, slug, BS_ZST_EXT, zst, zst_sz) ||
        !slot_file(dir, slug, BS_UNZ_EXT, scratch, sizeof(scratch)) ||
        !slot_file(dir, slug, BS_DB_EXT_S, legacy, sizeof(legacy)) || !cbm_mkdir_p(dir, 0755)) {
        return false;
    }
    unlink_scratch(scratch);
    bool ok = copy_db(db_path, scratch) && compress_file(scratch, zst);
    unlink_scratch(scratch);
    if (ok) {
        unlink_slot(legacy);
        *slot_bytes = file_size(zst);
    }
    return ok;
}

/* Remove scratch files a crashed park or restore left in dir. The caller
 * holds the index lease, so no other run is using them. */
static void sweep_scratch(const char *dir) {
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return;
    }
    char *names[64];
    int n = 0;
    cbm_dirent_t *ent;
    while (n < 64 && (ent = cbm_readdir(d)) != NULL) {
        if (!ent->is_dir && !slot_ext(ent->name) &&
            (is_unz_scratch(ent->name) || ends_with(ent->name, ".db.zst.tmp"))) {
            names[n] = strdup(ent->name);
            n += names[n] ? 1 : 0;
        }
    }
    cbm_closedir(d);
    for (int i = 0; i < n; i++) {
        char path[BS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        (void)cbm_unlink(path);
        free(names[i]);
    }
}

typedef struct {
    char name[CBM_DIRENT_NAME_MAX];
    int64_t mtime;
    int64_t bytes;
} bs_entry_t;

static int bs_entry_cmp_oldest(const void *a, const void *b) {
    const bs_entry_t *ea = (const bs_entry_t *)a;
    const bs_entry_t *eb = (const bs_entry_t *)b;
    if (ea->mtime != eb->mtime) {
        return ea->mtime < eb->mtime ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

/* The slot files in dir (both formats), with mtime and size. *n gets the
 * count; returns NULL when there are none or on allocation failure. */
static bs_entry_t *scan_slots(const char *dir, int *n) {
    *n = 0;
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return NULL;
    }
    int cap = 0;
    bs_entry_t *items = NULL;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir || !slot_ext(ent->name)) {
            continue;
        }
        if (*n == cap) {
            int next_cap = cap ? cap * 2 : 16;
            bs_entry_t *grown = (bs_entry_t *)realloc(items, (size_t)next_cap * sizeof(bs_entry_t));
            if (!grown) {
                break;
            }
            items = grown;
            cap = next_cap;
        }
        char path[BS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->name);
        bs_entry_t *e = &items[*n];
        snprintf(e->name, sizeof(e->name), "%s", ent->name);
        e->mtime = file_mtime(path);
        e->bytes = file_size(path);
        (*n)++;
    }
    cbm_closedir(d);
    return items;
}

/* Keep at most `keep` parked databases whose files total at most `budget`
 * bytes (budget <= 0: no size cap), dropping the least recently parked
 * first. The slot named `newest` (the one just parked, or NULL) is never
 * dropped, so a single slot larger than the budget still serves one switch
 * back. */
static void evict_slots(const char *dir, int keep, int64_t budget, const char *newest) {
    int n = 0;
    bs_entry_t *items = scan_slots(dir, &n);
    if (!items) {
        return;
    }
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += items[i].bytes > 0 ? items[i].bytes : 0;
    }
    qsort(items, (size_t)n, sizeof(bs_entry_t), bs_entry_cmp_oldest);
    int count = n;
    for (int i = 0; i < n && (count > keep || (budget > 0 && total > budget)); i++) {
        if (newest && strcmp(items[i].name, newest) == 0) {
            continue;
        }
        char path[BS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, items[i].name);
        unlink_slot(path);
        count--;
        total -= items[i].bytes > 0 ? items[i].bytes : 0;
        cbm_log_info("branch.evict", "slot", items[i].name);
    }
    free(items);
}

typedef enum { ADR_ABSENT = 0, ADR_PRESENT, ADR_UNREADABLE } adr_state_t;

/* The project's ADR (project_summaries) in the database at db_path. *content
 * (caller frees) is set only for ADR_PRESENT. */
static adr_state_t read_adr(const char *db_path, const char *project, char **content) {
    *content = NULL;
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) {
        return ADR_UNREADABLE;
    }
    cbm_adr_t adr = {0};
    int rc = cbm_store_adr_get(s, project, &adr);
    cbm_store_close(s);
    if (rc == CBM_STORE_NOT_FOUND) {
        return ADR_ABSENT;
    }
    if (rc != CBM_STORE_OK || !adr.content || !(*content = strdup(adr.content))) {
        cbm_store_adr_free(&adr);
        return ADR_UNREADABLE;
    }
    cbm_store_adr_free(&adr);
    return ADR_PRESENT;
}

/* The ADR is the one table in the database that is not derived from the
 * indexed source: manage_adr writes it for the project, whatever branch is
 * checked out, so the outgoing live database is the authority. Write its
 * state -- the ADR, or its absence (a delete leaves no row) -- into the slot
 * about to be restored, before the restore, so the restore installs both in
 * one transaction. False when the slot could not be brought in line -- and
 * when the outgoing ADR could not be read: the live database may then hold
 * the only copy of the newest ADR, so it must stay in place (the caller
 * keeps it as the seed) rather than be overwritten by the slot's older one. */
static bool carry_adr(const char *db_path, const char *project, const char *slot) {
    char *adr = NULL;
    adr_state_t state = read_adr(db_path, project, &adr);
    if (state == ADR_UNREADABLE) {
        cbm_log_warn("branch.adr_unreadable", "project", project);
        return false;
    }
    char *parked = NULL;
    adr_state_t parked_state = read_adr(slot, project, &parked);
    bool same = parked_state == state && (state == ADR_ABSENT || strcmp(parked, adr) == 0);
    free(parked);
    bool ok = same;
    if (!same) {
        cbm_store_t *s = cbm_store_open_path(slot);
        int rc = !s                     ? CBM_STORE_ERR
                 : state == ADR_PRESENT ? cbm_store_adr_store(s, project, adr)
                                        : cbm_store_adr_delete(s, project);
        ok = rc == CBM_STORE_OK || (state == ADR_ABSENT && rc == CBM_STORE_NOT_FOUND);
        cbm_store_close(s);
    }
    free(adr);
    return ok;
}

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*before_restore_cb)(void *) = nullptr;
static void *before_restore_data = nullptr;

void cbm_branch_slots_test_before_restore(void (*callback)(void *), void *data) {
    before_restore_cb = callback;
    before_restore_data = data;
}
#endif

/* Replace the CONTENT of the live database at db_path with the slot's, as
 * one ordinary SQLite write transaction (backup API into db_path's own
 * connection). The file, its WAL and its shared memory stay in place: a
 * reader mid-query keeps its snapshot, the next read transaction sees the
 * whole restored database, a crash before the commit leaves the outgoing
 * database intact, and nothing is renamed over a file another process has
 * open. Swapping the file in instead would leave a window -- and after a
 * crash, a permanent state -- in which db_path's old WAL sits beside the new
 * main file and is replayed onto it. The price is the copy going through the
 * WAL: the database's size written twice, and a WAL as large as the database
 * on disk until the restore's own checkpoint can reset it (shrink_wal) or
 * SQLite removes it at the last connection's close.
 *
 * That WAL peak is not bounded by stepping the backup in page batches: the
 * destination keeps one write transaction (and its lock) open from the first
 * step to the last, so every page is in the WAL before the commit (measured
 * with SQLite 3.53.1: 33 steps of 100 pages into a 216 MB database grew the
 * WAL steadily to 216 MB). Copying table by table in several transactions would bound it, but
 * readers would then see, and a crash would leave, a mix of two branches'
 * graphs. The single transaction is kept; see docs/BRANCH_INDEX.md.
 *
 * The ADR is carried INSIDE that transaction's write lock. The first backup
 * step takes db_path's write lock and holds it until the backup completes;
 * only then is the live ADR read (on another connection, which sees every
 * commit made before the lock) and written into the slot, and the rest of
 * the backup copies it. A manage_adr write from another process therefore
 * either committed before the lock and is carried, or waits on the lock and
 * lands on top of the restored database -- never lost in between. Writing
 * the slot restarts the copy (it is the backup's source), still under the
 * same lock.
 *
 * Fails, leaving db_path as it was, when the ADR cannot be carried, the page
 * sizes differ (a WAL database cannot change its page size) or the write lock
 * is not granted in time. */
/* After a restore, give the WAL's disk space back when no reader needs it.
 * The restore committed the whole database into the WAL. SQLite removes the
 * file at the last connection's close, but a process that keeps the database
 * open (a server's cached store) holds it at the database's size for as long
 * as it stays open. So: a PASSIVE checkpoint, which copies the frames into
 * the database file without blocking or waiting for anyone; if it copied
 * them all, one write transaction that rewrites the unchanged user_version
 * makes SQLite restart the WAL from its start and, under journal_size_limit
 * = 0, truncate the file to what that commit wrote. SQLite restarts the WAL
 * only when no reader is using its frames, so a reader mid-query keeps its
 * snapshot and the shrink waits. This is SQLite's own WAL reset, not a
 * TRUNCATE or RESTART checkpoint: nothing waits on readers, and the -wal file
 * is read with pread, never memory-mapped, so no process can fault on the
 * shorter file (cbm_store_checkpoint).
 *
 * Best effort: with readers that keep starting transactions it rarely finds
 * the WAL free (measured: 0-3% of restores on Linux, 5-15% on macOS, with
 * readers looping), and journal_size_limit = 0 lives only on this short-lived
 * connection. The backstop is the journal_size_limit every write connection
 * sets (cbm_store_resolve_journal_size_limit, 256 MiB): the next write after
 * a full checkpoint, on any connection, cuts the file back to it. */
static void shrink_wal(sqlite3 *db) {
    int frames = 0;
    int copied = 0;
    if (sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &frames, &copied) !=
            SQLITE_OK ||
        frames <= 0 || copied != frames) {
        return;
    }
    sqlite3_stmt *st = NULL;
    int version = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        version = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (version < 0) {
        return;
    }
    char sql[96];
    snprintf(sql, sizeof(sql), "PRAGMA journal_size_limit = 0; PRAGMA user_version = %d;",
             version);
    (void)sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static bool restore_into(const char *slot, const char *db_path, const char *project) {
#ifdef CBM_ENABLE_TEST_SEAMS
    if (before_restore_cb) {
        before_restore_cb(before_restore_data);
    }
#endif
    cbm_store_t *src = cbm_store_open_path_query(slot);
    cbm_store_t *dst = src ? cbm_store_open_path(db_path) : NULL;
    sqlite3 *sdb = cbm_store_get_db(src);
    sqlite3 *ddb = cbm_store_get_db(dst);
    bool ok = false;
    /* The caller unlinks the incoming slot once this returns: the commit must
     * be on stable storage by then. A write connection runs at synchronous =
     * NORMAL, under which a WAL commit is durable only at the next
     * checkpoint; FULL syncs the WAL at the commit. */
    if (ddb && sqlite3_exec(ddb, "PRAGMA synchronous = FULL;", NULL, NULL, NULL) != SQLITE_OK) {
        ddb = NULL;
    }
    sqlite3_backup *bk = sdb && ddb ? sqlite3_backup_init(ddb, "main", sdb, "main") : NULL;
    if (bk) {
        int rc = sqlite3_backup_step(bk, 1);
        if (rc == SQLITE_OK) {
            /* Write lock held, nothing committed yet. */
            if (carry_adr(db_path, project, slot)) {
                rc = sqlite3_backup_step(bk, -1);
                ok = rc == SQLITE_DONE;
            }
        } else if (rc == SQLITE_DONE) {
            /* A one-page database: no index database is that small (its
             * schema alone spans several pages), so nothing to carry into. */
            cbm_log_warn("branch.restore_tiny", "project", project);
            ok = true;
        }
        /* Before SQLITE_DONE, finish rolls the destination back. */
        ok = sqlite3_backup_finish(bk) == SQLITE_OK && ok;
    }
#ifdef CBM_ENABLE_TEST_SEAMS
    if (ok && on_event_cb) {
        sqlite3_stmt *st = NULL;
        char level[16] = "-1";
        if (sqlite3_prepare_v2(ddb, "PRAGMA synchronous;", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            snprintf(level, sizeof(level), "%d", sqlite3_column_int(st, 0));
        }
        sqlite3_finalize(st);
        bs_event("restore_commit", level);
    }
#endif
    if (ok) {
        shrink_wal(ddb);
    }
    cbm_store_close(dst);
    cbm_store_close(src);
    return ok;
}

typedef struct {
    int keep;
    int64_t budget;
} bs_limits_t;

static cbm_branch_switch_t switch_from(const char *db_path, const char *project, const char *slug,
                                       const char *indexed, const char *dir, const char *incoming,
                                       bs_limits_t lim);

/* Whether the database at path was indexed from root (cbm_root_matches): a
 * derived project name can be another directory's, and so can its slots. A
 * database with no project row or no recorded root is accepted. */
static bool db_root_matches(const char *path, const char *project, const char *root) {
    if (!root || !root[0]) {
        return true;
    }
    cbm_store_t *s = cbm_store_open_path_query(path);
    cbm_project_t proj = {0};
    bool ok = true;
    if (s && cbm_store_get_project(s, project, &proj) == CBM_STORE_OK) {
        ok = cbm_root_matches(proj.root_path, root);
        cbm_project_free_fields(&proj);
    }
    cbm_store_close(s);
    if (!ok) {
        cbm_log_warn("branch.foreign_db", "path", path, "root", root);
    }
    return ok;
}

/* Earlier builds parked slots directly under branches/<project>/, shared by
 * every root with that derived name, always uncompressed. Move such a slot
 * into this root's directory (as <slot>.db, `incoming`) when it records this
 * root; otherwise leave it (delete_project and the index reset still find
 * it). */
static bool adopt_legacy_slot(const char *db_path, const char *project, const char *root,
                              const char *slug, const char *dir, const char *incoming) {
    char legacy_dir[BS_PATH_MAX];
    char legacy[BS_PATH_MAX];
    if (!root || !root[0] ||
        !cbm_branch_slots_dir(db_path, project, NULL, legacy_dir, sizeof(legacy_dir)) ||
        !slot_file(legacy_dir, slug, BS_DB_EXT_S, legacy, sizeof(legacy)) ||
        !cbm_file_exists(legacy)) {
        return false;
    }
    cbm_store_t *s = cbm_store_open_path_query(legacy);
    cbm_project_t proj = {0};
    bool mine = s && cbm_store_get_project(s, project, &proj) == CBM_STORE_OK && proj.root_path &&
                proj.root_path[0] && cbm_root_matches(proj.root_path, root);
    cbm_project_free_fields(&proj);
    cbm_store_close(s);
    if (!mine || !cbm_mkdir_p(dir, 0755)) {
        return false;
    }
    cbm_remove_db_sidecars(incoming);
    if (cbm_rename_replace(legacy, incoming) != 0) {
        return false;
    }
    cbm_remove_db_sidecars(legacy);
    return true;
}

/* The incoming slot for slug in dir: <slot>.db.zst, else an uncompressed
 * <slot>.db an earlier build parked. False when neither exists. */
static bool find_slot(const char *dir, const char *slug, char *buf, size_t bufsz) {
    if (slot_file(dir, slug, BS_ZST_EXT, buf, bufsz) && cbm_file_exists(buf)) {
        return true;
    }
    return slot_file(dir, slug, BS_DB_EXT_S, buf, bufsz) && cbm_file_exists(buf);
}

/* The uncompressed database to install from `incoming`: the slot itself when
 * it is a legacy <slot>.db, else a decompressed scratch copy written to
 * `scratch` (the caller removes it with unlink_scratch). durable: the copy is
 * synced to stable storage, for a caller that renames it into place. */
static const char *open_incoming(const char *incoming, const char *dir, const char *slug,
                                 char *scratch, size_t scratch_sz, bool durable,
                                 int64_t *unpack_ms) {
    if (!ends_with(incoming, BS_ZST_EXT)) {
        return incoming;
    }
    uint64_t t0 = cbm_now_ms();
    if (!slot_file(dir, slug, BS_UNZ_EXT, scratch, scratch_sz)) {
        return NULL;
    }
    unlink_scratch(scratch);
    if (!decompress_file(incoming, scratch, durable)) {
        cbm_log_warn("branch.slot_unreadable", "slot", incoming);
        unlink_scratch(scratch);
        return NULL;
    }
    *unpack_ms = (int64_t)(cbm_now_ms() - t0);
    return scratch;
}

cbm_branch_switch_t cbm_branch_slots_switch(const char *db_path, const char *project,
                                            const char *slug, const char *root, int keep,
                                            int64_t budget, char **from) {
    if (from) {
        *from = NULL;
    }
    char dir[BS_PATH_MAX];
    char incoming[BS_PATH_MAX];
    if (keep < 1 || !slug_is_safe(slug) ||
        !cbm_branch_slots_dir(db_path, project, root, dir, sizeof(dir))) {
        return CBM_BRANCH_SWITCH_NONE;
    }
    sweep_scratch(dir);
    bool have_incoming = find_slot(dir, slug, incoming, sizeof(incoming));
    if (!have_incoming && slot_file(dir, slug, BS_DB_EXT_S, incoming, sizeof(incoming))) {
        have_incoming = adopt_legacy_slot(db_path, project, root, slug, dir, incoming);
    }

    /* No current database (first index here, an interrupted full rebuild, a
     * manual delete): this branch's parked database is a better start than
     * nothing, and the incremental pass corrects whatever moved since it was
     * parked. With no main file there is no reader to protect, so it is
     * installed the way a fresh database is (stale sidecars removed, then one
     * rename): a compressed slot is decompressed beside it and renamed in, a
     * legacy one copied. The slot is removed only once the installed file and
     * its directory entry are on stable storage: a power loss in between
     * must not leave neither. */
    if (!cbm_file_exists(db_path)) {
        if (!have_incoming) {
            return CBM_BRANCH_SWITCH_NONE;
        }
        char scratch[BS_PATH_MAX];
        int64_t unpack_ms = 0;
        const char *src =
            open_incoming(incoming, dir, slug, scratch, sizeof(scratch), true, &unpack_ms);
        bool ok = false;
        bool durable = false;
        if (src == incoming) {
            ok = copy_db(incoming, db_path);
            durable = ok && sync_file(db_path);
        } else if (src) {
            cbm_remove_db_sidecars(db_path);
            ok = cbm_rename_replace(src, db_path) == 0;
            if (ok) {
                bs_event("rename", db_path);
            }
            durable = ok;
            unlink_scratch(scratch);
        }
        if (ok) {
            sync_parent_dir(db_path);
            if (durable) {
                bs_event("unlink_incoming", incoming);
                unlink_slot(incoming);
            }
            cbm_log_info("branch.switch", "action", "restored", "to", slug, "from", "");
            return CBM_BRANCH_SWITCH_RESTORED;
        }
        return CBM_BRANCH_SWITCH_NONE;
    }

    char *indexed = read_indexed_slug(db_path, project);
    if (!indexed) {
        /* No Branch node: a non-git index or an unreadable file. Leave it to
         * the normal routing, which rebuilds what it cannot use. */
        return CBM_BRANCH_SWITCH_NONE;
    }
    /* Slots live under this root's own directory, so every slot found there
     * is this checkout's. The live database is shared by name, though: one
     * from another directory (a derived-name collision) must not be parked
     * among this root's slots. It is left to the normal routing, which
     * reindexes over it as it always has. Checked only when a switch is due,
     * so a run on the same branch opens nothing extra. */
    cbm_branch_switch_t action = CBM_BRANCH_SWITCH_NONE;
    if (strcmp(indexed, slug) != 0 && db_root_matches(db_path, project, root)) {
        bs_limits_t lim = {keep, budget};
        action = switch_from(db_path, project, slug, indexed, dir,
                             have_incoming ? incoming : NULL, lim);
    }
    if (from) {
        *from = indexed;
    } else {
        free(indexed);
    }
    return action;
}

/* The switch proper, once the database is known to be another branch's.
 * incoming is the slot to restore, NULL when none is parked. */
static cbm_branch_switch_t switch_from(const char *db_path, const char *project, const char *slug,
                                       const char *indexed, const char *dir, const char *incoming,
                                       bs_limits_t lim) {
    char outgoing[BS_PATH_MAX];
    uint64_t t0 = cbm_now_ms();
    int64_t slot_bytes = -1;
    if (!park_db(db_path, dir, indexed, outgoing, sizeof(outgoing), &slot_bytes)) {
        cbm_log_warn("branch.park_failed", "from", indexed, "to", slug, "action", "in_place");
        return CBM_BRANCH_SWITCH_IN_PLACE;
    }
    log_kv("branch.park", "elapsed_ms", (int64_t)(cbm_now_ms() - t0), "slot_bytes", slot_bytes,
           "db_bytes", file_size(db_path));
    /* Two slugs can name one slot file: on a case-insensitive filesystem
     * "Feature" and "feature" do. The park has then just overwritten the
     * incoming slot (or removed it, as the outgoing branch's legacy .db), so
     * it holds the outgoing graph, not the incoming one. Restoring it would
     * report a restore of nothing and unlink the only park. */
    if (incoming && (!cbm_file_exists(incoming) || same_file(incoming, outgoing))) {
        cbm_log_warn("branch.slot_collision", "from", indexed, "to", slug, "slot", outgoing);
        incoming = NULL;
    }

    /* Restore by content (restore_into). A failure at any step leaves the
     * outgoing database in place, already parked: it becomes the seed, as if
     * nothing had been parked for this branch.
     *
     * Ordered for a power loss at any point: the park is on stable storage
     * (compress_file syncs the slot and its directory) before the restore
     * overwrites the outgoing graph, the restore's commit is synced before
     * restore_into returns (synchronous = FULL), and only then is the
     * incoming slot removed. Each branch's graph is always in the live
     * database or in its slot. */
    cbm_branch_switch_t action = CBM_BRANCH_SWITCH_SEEDED;
    if (incoming) {
        t0 = cbm_now_ms();
        char scratch[BS_PATH_MAX];
        int64_t unpack_ms = 0;
        const char *src =
            open_incoming(incoming, dir, slug, scratch, sizeof(scratch), false, &unpack_ms);
        if (src && restore_into(src, db_path, project)) {
            action = CBM_BRANCH_SWITCH_RESTORED;
            log_kv("branch.restore", "elapsed_ms", (int64_t)(cbm_now_ms() - t0), "unpack_ms",
                   unpack_ms, "wal_bytes", wal_size(db_path));
            bs_event("unlink_incoming", incoming);
            unlink_slot(incoming);
        } else {
            cbm_log_warn("branch.restore_failed", "to", slug, "action", "seeded");
        }
        if (src && src != incoming) {
            unlink_scratch(scratch);
        }
    }
    const char *newest = strrchr(outgoing, '/');
    evict_slots(dir, lim.keep, lim.budget, newest ? newest + 1 : outgoing);
    cbm_log_info("branch.switch", "action", cbm_branch_switch_str(action), "from", indexed, "to",
                 slug);
    return action;
}

typedef struct {
    cbm_branch_slot_info_t *items;
    int n;
    int cap;
} bs_info_list_t;

static char *join2(const char *a, const char *b, size_t blen) {
    size_t alen = strlen(a);
    char *out = (char *)malloc(alen + blen + 1);
    if (out) {
        memcpy(out, a, alen);
        memcpy(out + alen, b, blen);
        out[alen + blen] = '\0';
    }
    return out;
}

/* Append the slot files in dir, each name prefixed with prefix. */
static void info_dir_slots(const char *dir, const char *prefix, bs_info_list_t *l) {
    int n = 0;
    bs_entry_t *items = scan_slots(dir, &n);
    for (int i = 0; i < n; i++) {
        if (l->n == l->cap) {
            int next_cap = l->cap ? l->cap * 2 : 8;
            cbm_branch_slot_info_t *grown = (cbm_branch_slot_info_t *)realloc(
                l->items, (size_t)next_cap * sizeof(cbm_branch_slot_info_t));
            if (!grown) {
                break;
            }
            l->items = grown;
            l->cap = next_cap;
        }
        const char *ext = slot_ext(items[i].name);
        cbm_branch_slot_info_t *e = &l->items[l->n];
        e->name = join2(prefix, items[i].name, strlen(items[i].name) - strlen(ext));
        e->file = join2(prefix, items[i].name, strlen(items[i].name));
        e->bytes = items[i].bytes;
        e->compressed = ext == BS_ZST_EXT;
        if (!e->name || !e->file) {
            free(e->name);
            free(e->file);
            break;
        }
        l->n++;
    }
    free(items);
}

/* The root-key subdirectories of a project's slot directory. */
static int list_root_dirs(const char *pdir, char ***out) {
    *out = NULL;
    cbm_dir_t *d = cbm_opendir(pdir);
    if (!d) {
        return 0;
    }
    int n = 0;
    int cap = 0;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (!ent->is_dir || ent->name[0] == '.') {
            continue;
        }
        if (n == cap) {
            int next_cap = cap ? cap * 2 : 4;
            char **grown = (char **)realloc(*out, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            *out = grown;
            cap = next_cap;
        }
        if (!((*out)[n] = strdup(ent->name))) {
            break;
        }
        n++;
    }
    cbm_closedir(d);
    return n;
}

int cbm_branch_slots_info(const char *db_path, const char *project, const char *root,
                          cbm_branch_slot_info_t **out) {
    if (out) {
        *out = NULL;
    }
    char dir[BS_PATH_MAX];
    if (!out || !cbm_branch_slots_dir(db_path, project, root, dir, sizeof(dir))) {
        return 0;
    }
    bs_info_list_t l = {NULL, 0, 0};
    info_dir_slots(dir, "", &l);
    if (!root || !root[0]) {
        /* Every root's slots, as "<root-key>/<slot>" (plus legacy ones). */
        char **keys = NULL;
        int nk = list_root_dirs(dir, &keys);
        for (int i = 0; i < nk; i++) {
            char sub[BS_PATH_MAX];
            char prefix[CBM_DIRENT_NAME_MAX + 2];
            snprintf(sub, sizeof(sub), "%s/%s", dir, keys[i]);
            snprintf(prefix, sizeof(prefix), "%s/", keys[i]);
            info_dir_slots(sub, prefix, &l);
            free(keys[i]);
        }
        free(keys);
    }
    if (l.n > 1) {
        qsort(l.items, (size_t)l.n, sizeof(cbm_branch_slot_info_t), [](const void *a, const void *b) {
            return strcmp(((const cbm_branch_slot_info_t *)a)->file,
                          ((const cbm_branch_slot_info_t *)b)->file);
        });
    }
    *out = l.items;
    return l.n;
}

void cbm_branch_slots_free_info(cbm_branch_slot_info_t *info, int count) {
    for (int i = 0; i < count; i++) {
        free(info[i].name);
        free(info[i].file);
    }
    free(info);
}

int cbm_branch_slots_list(const char *db_path, const char *project, const char *root, char ***out) {
    if (out) {
        *out = NULL;
    }
    cbm_branch_slot_info_t *info = NULL;
    int n = out ? cbm_branch_slots_info(db_path, project, root, &info) : 0;
    char **list = n > 0 ? (char **)malloc((size_t)n * sizeof(char *)) : NULL;
    int m = 0;
    for (int i = 0; list && i < n; i++) {
        list[m++] = info[i].name;
        info[i].name = NULL;
    }
    cbm_branch_slots_free_info(info, n);
    if (m > 1) {
        qsort(list, (size_t)m, sizeof(char *), [](const void *a, const void *b) {
            return strcmp(*(const char *const *)a, *(const char *const *)b);
        });
        /* One entry per branch: a crash between a park and its removal of
         * the superseded uncompressed slot can leave both formats. */
        int w = 1;
        for (int i = 1; i < m; i++) {
            if (strcmp(list[i], list[w - 1]) == 0) {
                free(list[i]);
            } else {
                list[w++] = list[i];
            }
        }
        m = w;
    }
    if (out) {
        *out = list;
    }
    return m;
}

void cbm_branch_slots_free_list(char **list, int count) {
    for (int i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

/* Delete the slot files in dir (this module's names only). */
static int delete_dir_slots(const char *dir) {
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return 0;
    }
    /* Collect first: unlinking while iterating is unspecified on some
     * platforms' readdir. */
    int removed = 0;
    int cap = 0;
    int n = 0;
    char **names = NULL;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir) {
            continue;
        }
        if (n == cap) {
            int next_cap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(names, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            names = grown;
            cap = next_cap;
        }
        size_t len = strlen(ent->name);
        names[n] = (char *)malloc(len + 1);
        if (!names[n]) {
            break;
        }
        memcpy(names[n], ent->name, len + 1);
        n++;
    }
    cbm_closedir(d);
    for (int i = 0; i < n; i++) {
        /* Only what this module writes: <slot>.db.zst or a legacy
         * <slot>.db, and the scratch and temp files beside them. Anything
         * else in the directory is left. */
        const char *name = names[i];
        bool is_db = slot_ext(name) != NULL;
        bool is_side = is_scratch_name(name);
        if (is_db || is_side) {
            char path[BS_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, name);
            if (cbm_unlink(path) == 0 && is_db) {
                removed++;
            }
        }
        free(names[i]);
    }
    free(names);
    return removed;
}

int cbm_branch_slots_delete_all(const char *db_path, const char *project) {
    char dir[BS_PATH_MAX];
    if (!cbm_branch_slots_dir(db_path, project, NULL, dir, sizeof(dir))) {
        return 0;
    }
    /* Every root's slots (a derived name can be several directories'), and
     * any legacy ones parked directly in the project directory. */
    int removed = delete_dir_slots(dir);
    char **keys = NULL;
    int nk = list_root_dirs(dir, &keys);
    for (int i = 0; i < nk; i++) {
        char sub[BS_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", dir, keys[i]);
        removed += delete_dir_slots(sub);
        (void)cbm_rmdir(sub);
        free(keys[i]);
    }
    free(keys);
    (void)cbm_rmdir(dir);
    /* The shared parent goes too once no project has parked anything. */
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)cbm_rmdir(dir);
    }
    return removed;
}

/* Expire the files of one slot directory (cbm_branch_slots_expire). */
static int expire_dir(const char *dir, int64_t slot_before, int64_t scratch_before, bool dry_run,
                      cbm_branch_slot_expire_cb cb, void *ud) {
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return 0;
    }
    int cap = 0;
    int n = 0;
    char **names = NULL;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (ent->is_dir || !(slot_ext(ent->name) || is_scratch_name(ent->name))) {
            continue;
        }
        if (n == cap) {
            int next_cap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(names, (size_t)next_cap * sizeof(char *));
            if (!grown) {
                break;
            }
            names = grown;
            cap = next_cap;
        }
        if (!(names[n] = strdup(ent->name))) {
            break;
        }
        n++;
    }
    cbm_closedir(d);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        char path[BS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        /* A slot's own SQLite sidecars (legacy .db slots) go with the slot;
         * they are not judged on their own while it exists. Once it is gone
         * (an interrupted delete) nothing else removes them: they are
         * scratch, judged by the scratch age. */
        bool is_slot = slot_ext(names[i]) != NULL;
        bool sidecar = false;
        static const char *const sidecar_ext[] = {"-wal", "-shm", "-journal"};
        for (const char *ext : sidecar_ext) {
            size_t nlen = strlen(names[i]);
            size_t elen = strlen(ext);
            if (is_slot || is_unz_scratch(names[i]) || nlen <= elen || !ends_with(names[i], ext)) {
                continue;
            }
            char base[BS_PATH_MAX];
            snprintf(base, sizeof(base), "%s/%.*s", dir, (int)(nlen - elen), names[i]);
            sidecar = ends_with(base, BS_DB_EXT_S) && cbm_file_exists(base);
            break;
        }
        int64_t before = is_slot ? slot_before : scratch_before;
        int64_t mt = file_mtime(path);
        if (sidecar || before <= 0 || mt < 0 || mt >= before) {
            free(names[i]);
            continue;
        }
        int64_t bytes = file_size(path);
        /* Reported and counted only once the file is gone: a refused unlink
         * (a read-only cache, a Windows sharing violation) would otherwise
         * claim space the file still holds. */
        bool gone = dry_run;
        if (!dry_run) {
            gone = is_slot ? unlink_slot(path) : (cbm_unlink(path) == 0 || !cbm_file_exists(path));
            if (gone && is_slot) {
                cbm_log_info("cache.expire", "kind", "slot", "path", path);
            } else if (!gone) {
                cbm_log_warn("cache.expire.skipped", "kind", is_slot ? "slot" : "scratch", "path",
                             path, "reason", "unlink_failed");
            }
        }
        if (gone) {
            if (cb) {
                cb(path, bytes, !is_slot, ud);
            }
            removed += is_slot ? 1 : 0;
        }
        free(names[i]);
    }
    free(names);
    return removed;
}

int cbm_branch_slots_expire(const char *db_path, const char *project, int64_t slot_before,
                            int64_t scratch_before, bool dry_run, cbm_branch_slot_expire_cb cb,
                            void *ud) {
    char dir[BS_PATH_MAX];
    if (!cbm_branch_slots_dir(db_path, project, NULL, dir, sizeof(dir))) {
        return 0;
    }
    int removed = expire_dir(dir, slot_before, scratch_before, dry_run, cb, ud);
    char **keys = NULL;
    int nk = list_root_dirs(dir, &keys);
    for (int i = 0; i < nk; i++) {
        char sub[BS_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", dir, keys[i]);
        removed += expire_dir(sub, slot_before, scratch_before, dry_run, cb, ud);
        if (!dry_run) {
            (void)cbm_rmdir(sub); /* only when empty */
        }
        free(keys[i]);
    }
    free(keys);
    if (!dry_run) {
        (void)cbm_rmdir(dir);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            (void)cbm_rmdir(dir);
        }
    }
    return removed;
}
