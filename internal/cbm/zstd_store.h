#ifndef CBM_ZSTD_STORE_H
#define CBM_ZSTD_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Zstd compression at specified level (1=fast .. 22=best).
// srcLen/dstCap are size_t for the same reason decompression's are: a >2 GiB
// database would otherwise wrap through int and hand the encoder a capacity that
// disagrees with the real buffer. Returns compressed size on success, 0 on error.

#ifdef __cplusplus
extern "C" {
#endif

int64_t cbm_zstd_compress(const char *src, size_t srcLen, char *dst, size_t dstCap, int level);

// Zstd decompression. srcLen/dstCap are size_t so a >2 GiB destination capacity
// is never truncated (a large DB artifact, or a crafted one, would otherwise
// wrap through int and desync the real buffer size from the decoder capacity).
// Returns decompressed size on success (>0), 0 on error.
int64_t cbm_zstd_decompress(const char *src, size_t srcLen, char *dst, size_t dstCap);

// Declared decompressed content size of a zstd frame (from its header), or 0
// when unknown/unreadable. Lets the caller size the destination from the frame
// itself instead of trusting a separate, attacker-controllable size field.
size_t cbm_zstd_frame_content_size(const char *src, size_t srcLen);

// Maximum compressed size bound for given input size.
size_t cbm_zstd_compress_bound(size_t inputSize);

// Streaming file forms, for inputs too large to hold in memory (a parked
// branch database can be several GB). Both read `in` to EOF and write to `out`
// through fixed-size buffers; neither seeks, closes or flushes the FILEs
// beyond writing.
//
// cbm_zstd_compress_stream writes ONE frame with a content checksum and, when
// src_size >= 0, the content size in its header. `workers` > 1 asks for
// multithreaded compression (ignored when the library was built without it).
// Returns the compressed byte count, or -1 on any read, write or zstd error.
int64_t cbm_zstd_compress_stream(FILE *in, FILE *out, int64_t src_size, int level, int workers);

// cbm_zstd_decompress_stream accepts exactly one complete frame: a truncated
// or corrupt frame, a checksum mismatch, trailing bytes, or more than max_out
// decompressed bytes all fail. Returns the decompressed byte count, or -1.
int64_t cbm_zstd_decompress_stream(FILE *in, FILE *out, uint64_t max_out);

#ifdef __cplusplus
}
#endif

#endif // CBM_ZSTD_STORE_H
