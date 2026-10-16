#ifndef CBM_SHA256_H
#define CBM_SHA256_H

/* In-process SHA-256 (FIPS 180-4). Used to verify the integrity of a
 * downloaded release before installing it, without shelling out to a
 * platform hashing tool (shasum / sha256sum / certutil) — those differ per
 * OS, may be absent, and mis-quote paths under cmd.exe. */

#include <stddef.h>
#include <stdint.h>

#define CBM_SHA256_DIGEST_LEN 32 /* raw digest bytes */
#define CBM_SHA256_HEX_LEN 64    /* lowercase hex chars (no NUL) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
    /* Message-schedule scratch for the block transform. Kept here rather than
     * as a 256-byte local in the transform: a local that size is fake-stacked
     * by ASan's use-after-return mode on EVERY 64-byte block, turning one hash
     * of a large file into millions of allocations. Allocated once per hash. */
    uint32_t sched[64];
} cbm_sha256_ctx;

void cbm_sha256_init(cbm_sha256_ctx *c);
void cbm_sha256_update(cbm_sha256_ctx *c, const void *data, size_t len);
void cbm_sha256_final(cbm_sha256_ctx *c, uint8_t out[CBM_SHA256_DIGEST_LEN]);

/* One-shot hash of a buffer to lowercase hex. `out` must hold
 * CBM_SHA256_HEX_LEN + 1 bytes (hex chars + NUL). */
void cbm_sha256_hex(const void *data, size_t len, char out[CBM_SHA256_HEX_LEN + 1]);

#ifdef CBM_ENABLE_TEST_SEAMS
/* The block backend in use: "arm-sha2", "x86-sha-ni" or "portable". */
const char *cbm_sha256_backend_name_for_testing(void);
/* Force the portable transform (true) or return to the probed backend (false),
 * so a test can hash the same input both ways. Process-wide. */
void cbm_sha256_force_portable_for_testing(bool force);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CBM_SHA256_H */
