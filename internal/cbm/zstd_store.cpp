// zstd_store.c — Thin C wrappers around Zstandard.

#include "vendored/zstd/zstd.h"
#include "vendored/zstd/zstd_errors.h"

#include "zstd_store.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int64_t cbm_zstd_compress(const char *src, size_t srcLen, char *dst, size_t dstCap, int level) {
    size_t rc = ZSTD_compress(dst, dstCap, src, srcLen, level);
    if (ZSTD_isError(rc)) {
        return 0;
    }
    return (int64_t)rc;
}

int64_t cbm_zstd_decompress(const char *src, size_t srcLen, char *dst, size_t dstCap) {
    size_t rc = ZSTD_decompress(dst, dstCap, src, srcLen);
    if (ZSTD_isError(rc)) {
        return 0;
    }
    return (int64_t)rc;
}

size_t cbm_zstd_frame_content_size(const char *src, size_t srcLen) {
    unsigned long long n = ZSTD_getFrameContentSize(src, srcLen);
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR) {
        return 0;
    }
    return (size_t)n;
}

size_t cbm_zstd_compress_bound(size_t inputSize) {
    return ZSTD_compressBound(inputSize);
}

int64_t cbm_zstd_compress_stream(FILE *in, FILE *out, int64_t src_size, int level, int workers) {
    if (!in || !out) {
        return -1;
    }
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    size_t in_cap = ZSTD_CStreamInSize();
    size_t out_cap = ZSTD_CStreamOutSize();
    char *ibuf = (char *)malloc(in_cap);
    char *obuf = (char *)malloc(out_cap);
    int64_t written = -1;
    if (cctx && ibuf && obuf &&
        !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)) &&
        !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1)) &&
        (src_size < 0 ||
         !ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(cctx, (unsigned long long)src_size)))) {
        if (workers > 1) {
            /* An error here means a single-threaded build: compress inline. */
            (void)ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers);
        }
        written = 0;
        bool done = false;
        while (!done && written >= 0) {
            size_t got = fread(ibuf, 1, in_cap, in);
            if (got < in_cap && ferror(in)) {
                written = -1;
                break;
            }
            bool last = got < in_cap;
            ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
            ZSTD_inBuffer input = {ibuf, got, 0};
            bool finished = false;
            while (!finished) {
                ZSTD_outBuffer output = {obuf, out_cap, 0};
                size_t remaining = ZSTD_compressStream2(cctx, &output, &input, mode);
                if (ZSTD_isError(remaining) ||
                    (output.pos && fwrite(obuf, 1, output.pos, out) != output.pos)) {
                    written = -1;
                    break;
                }
                written += (int64_t)output.pos;
                finished = last ? remaining == 0 : input.pos == input.size;
            }
            done = last;
        }
    }
    free(ibuf);
    free(obuf);
    ZSTD_freeCCtx(cctx);
    return written;
}

int64_t cbm_zstd_decompress_stream(FILE *in, FILE *out, uint64_t max_out) {
    return cbm_zstd_decompress_stream_ex(in, out, max_out, NULL);
}

int64_t cbm_zstd_decompress_stream_ex(FILE *in, FILE *out, uint64_t max_out, bool *data_error) {
    if (data_error) {
        *data_error = false;
    }
    if (!in || !out) {
        return -1;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t in_cap = ZSTD_DStreamInSize();
    size_t out_cap = ZSTD_DStreamOutSize();
    char *ibuf = (char *)malloc(in_cap);
    char *obuf = (char *)malloc(out_cap);
    uint64_t total = 0;
    bool ok = dctx && ibuf && obuf;
    bool bad_data = false; /* the input itself is at fault, not I/O or memory */
    size_t last_ret = 1;
    bool frame_done = false;
    while (ok) {
        size_t got = fread(ibuf, 1, in_cap, in);
        if (got == 0) {
            ok = !ferror(in);
            break;
        }
        if (frame_done) {
            ok = false; /* bytes after the one frame */
            bad_data = true;
            break;
        }
        ZSTD_inBuffer input = {ibuf, got, 0};
        for (;;) {
            ZSTD_outBuffer output = {obuf, out_cap, 0};
            last_ret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(last_ret)) {
                ok = false;
                bad_data = ZSTD_getErrorCode(last_ret) != ZSTD_error_memory_allocation;
                break;
            }
            total += output.pos;
            if (total > max_out) {
                ok = false;
                bad_data = true;
                break;
            }
            if (output.pos && fwrite(obuf, 1, output.pos, out) != output.pos) {
                ok = false;
                break;
            }
            if (last_ret == 0) {
                frame_done = true;
                ok = input.pos == input.size; /* trailing bytes in this read */
                bad_data = !ok;
                break;
            }
            /* A full output buffer may leave decoded bytes inside the
             * decoder: call again before reading more input. */
            if (input.pos == input.size && output.pos < output.size) {
                break;
            }
        }
    }
    if (ok && !frame_done) {
        ok = false; /* EOF inside the frame: truncated */
        bad_data = true;
    }
    free(ibuf);
    free(obuf);
    ZSTD_freeDCtx(dctx);
    if (data_error) {
        *data_error = !ok && bad_data;
    }
    return ok ? (int64_t)total : -1;
}
