/* SHA-256 per FIPS 180-4. Straightforward reference implementation; validated
 * against the NIST test vectors in tests/test_cli.cpp.
 *
 * Block compression runs on the CPU's SHA-256 instructions when it has them
 * (ARMv8 SHA2, x86 SHA-NI), chosen once at runtime; the portable transform
 * below stays the fallback and the reference the tests compare against.
 * Upstream #2441: every process start fingerprints its own large executable,
 * and the portable transform made that over a second of CPU. */

#include "foundation/sha256.h"

#include <atomic>
#include <string.h>

/* The ARM path needs a way to ask the OS whether the CPU has SHA2; on any
 * other aarch64 OS the portable transform is used. */
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__)) && \
    (defined(__APPLE__) || defined(__linux__) || defined(_WIN32))
#define SHA256_HW_ARM 1
#include <arm_neon.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1UL << 6)
#endif
#else
/* Only for IsProcessorFeaturePresent; keep min/max and friends out. */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(__clang__)
#define SHA256_ARM_TARGET __attribute__((target("sha2")))
#else
/* "+crypto", not "+sha2": GCC 11's arm_neon.h declares the SHA2 intrinsics
 * under "+nothing+crypto", so a "+sha2" function cannot inline them ("target
 * specific option mismatch"). "+crypto" implies "+sha2" on every GCC; the
 * function uses only SHA2 instructions and the runtime HWCAP check gates it. */
#define SHA256_ARM_TARGET __attribute__((target("+crypto")))
#endif
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#define SHA256_HW_X86 1
#include <cpuid.h>
#include <immintrin.h>
#define SHA256_X86_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#endif

namespace {
constexpr size_t SHA256_BLOCK_BYTES = 64;
} // namespace

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(cbm_sha256_ctx *c, const uint8_t *data) {
    /* Scratch comes from the context, not this frame (see cbm_sha256_ctx::sched). */
    uint32_t *m = c->sched;
    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, cc);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
}

static void sha256_blocks_portable(cbm_sha256_ctx *c, const uint8_t *data, size_t blocks) {
    for (; blocks > 0; blocks--, data += SHA256_BLOCK_BYTES) {
        sha256_transform(c, data);
    }
}

#if defined(SHA256_HW_ARM)
/* ARMv8 SHA2: the state stays {a,b,c,d} / {e,f,g,h}; each SHA256H/H2 pair runs
 * four rounds, and SU0/SU1 extend the schedule four words at a time. */
SHA256_ARM_TARGET static void sha256_blocks_arm(cbm_sha256_ctx *c, const uint8_t *data,
                                                size_t blocks) {
    uint32x4_t abcd = vld1q_u32(&c->state[0]);
    uint32x4_t efgh = vld1q_u32(&c->state[4]);
    for (; blocks > 0; blocks--, data += SHA256_BLOCK_BYTES) {
        uint32x4_t abcd_in = abcd;
        uint32x4_t efgh_in = efgh;
        uint32x4_t w[4];
        for (int i = 0; i < 4; i++) {
            w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + i * 16)));
        }
        for (int i = 0; i < 16; i++) {
            uint32x4_t wk = vaddq_u32(w[i & 3], vld1q_u32(&K[i * 4]));
            uint32x4_t abcd_prev = abcd;
            abcd = vsha256hq_u32(abcd, efgh, wk);
            efgh = vsha256h2q_u32(efgh, abcd_prev, wk);
            if (i < 12) {
                w[i & 3] = vsha256su1q_u32(vsha256su0q_u32(w[i & 3], w[(i + 1) & 3]),
                                           w[(i + 2) & 3], w[(i + 3) & 3]);
            }
        }
        abcd = vaddq_u32(abcd, abcd_in);
        efgh = vaddq_u32(efgh, efgh_in);
    }
    vst1q_u32(&c->state[0], abcd);
    vst1q_u32(&c->state[4], efgh);
}

static bool sha256_cpu_has_arm() {
#if defined(__APPLE__)
    /* Present on every Apple arm64 CPU; the key itself exists since macOS 12. */
    int has = 0;
    size_t size = sizeof(has);
    return sysctlbyname("hw.optional.arm.FEAT_SHA256", &has, &size, nullptr, 0) == 0 && has != 0;
#elif defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
    return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != 0;
#endif
}
#endif

#if defined(SHA256_HW_X86)
static inline __m128i sha256_load128(const void *p) {
    return _mm_loadu_si128(static_cast<const __m128i *>(p));
}

/* x86 SHA-NI: SHA256RNDS2 wants the state as {a,b,e,f} / {c,d,g,h}, so it is
 * shuffled in once per call and back out at the end. */
SHA256_X86_TARGET static void sha256_blocks_x86(cbm_sha256_ctx *c, const uint8_t *data,
                                                size_t blocks) {
    const __m128i byteswap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i dcba = _mm_shuffle_epi32(sha256_load128(&c->state[0]), 0xB1);
    __m128i efgh = _mm_shuffle_epi32(sha256_load128(&c->state[4]), 0x1B);
    __m128i abef = _mm_alignr_epi8(dcba, efgh, 8);
    __m128i cdgh = _mm_blend_epi16(efgh, dcba, 0xF0);
    for (; blocks > 0; blocks--, data += SHA256_BLOCK_BYTES) {
        __m128i abef_in = abef;
        __m128i cdgh_in = cdgh;
        __m128i w[4];
        for (int i = 0; i < 4; i++) {
            w[i] = _mm_shuffle_epi8(sha256_load128(data + i * 16), byteswap);
        }
        for (int i = 0; i < 16; i++) {
            __m128i wk = _mm_add_epi32(w[i & 3], sha256_load128(&K[i * 4]));
            cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
            abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0x0E));
            if (i < 12) {
                __m128i next = _mm_add_epi32(_mm_sha256msg1_epu32(w[i & 3], w[(i + 1) & 3]),
                                             _mm_alignr_epi8(w[(i + 3) & 3], w[(i + 2) & 3], 4));
                w[i & 3] = _mm_sha256msg2_epu32(next, w[(i + 3) & 3]);
            }
        }
        abef = _mm_add_epi32(abef, abef_in);
        cdgh = _mm_add_epi32(cdgh, cdgh_in);
    }
    __m128i feba = _mm_shuffle_epi32(abef, 0x1B);
    __m128i dchg = _mm_shuffle_epi32(cdgh, 0xB1);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&c->state[0]), _mm_blend_epi16(feba, dchg, 0xF0));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&c->state[4]), _mm_alignr_epi8(dchg, feba, 8));
}

static bool sha256_cpu_has_x86() {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    bool ssse3 = (ecx & (1U << 9)) != 0;
    bool sse41 = (ecx & (1U << 19)) != 0;
    if (!ssse3 || !sse41 || !__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    return (ebx & (1U << 29)) != 0; /* CPUID.(7,0):EBX.SHA */
}
#endif

namespace {

enum class Sha256Backend : int { Unprobed = 0, Portable, Arm, X86 };

/* Probed once; every thread computes the same answer, so relaxed is enough. */
std::atomic<int> g_sha256_backend{static_cast<int>(Sha256Backend::Unprobed)};
std::atomic<bool> g_sha256_force_portable{false};

Sha256Backend sha256_probe_backend() {
#if defined(SHA256_HW_ARM)
    if (sha256_cpu_has_arm()) {
        return Sha256Backend::Arm;
    }
#elif defined(SHA256_HW_X86)
    if (sha256_cpu_has_x86()) {
        return Sha256Backend::X86;
    }
#endif
    return Sha256Backend::Portable;
}

Sha256Backend sha256_backend() {
    if (g_sha256_force_portable.load(std::memory_order_relaxed)) {
        return Sha256Backend::Portable;
    }
    int backend = g_sha256_backend.load(std::memory_order_relaxed);
    if (backend == static_cast<int>(Sha256Backend::Unprobed)) {
        backend = static_cast<int>(sha256_probe_backend());
        g_sha256_backend.store(backend, std::memory_order_relaxed);
    }
    return static_cast<Sha256Backend>(backend);
}

void sha256_blocks(cbm_sha256_ctx *c, const uint8_t *data, size_t blocks) {
    switch (sha256_backend()) {
#if defined(SHA256_HW_ARM)
    case Sha256Backend::Arm:
        sha256_blocks_arm(c, data, blocks);
        return;
#endif
#if defined(SHA256_HW_X86)
    case Sha256Backend::X86:
        sha256_blocks_x86(c, data, blocks);
        return;
#endif
    default:
        sha256_blocks_portable(c, data, blocks);
        return;
    }
}

} // namespace

#ifdef CBM_ENABLE_TEST_SEAMS
const char *cbm_sha256_backend_name_for_testing(void) {
    switch (sha256_backend()) {
    case Sha256Backend::Arm:
        return "arm-sha2";
    case Sha256Backend::X86:
        return "x86-sha-ni";
    default:
        return "portable";
    }
}

void cbm_sha256_force_portable_for_testing(bool force) {
    g_sha256_force_portable.store(force, std::memory_order_relaxed);
}
#endif

void cbm_sha256_init(cbm_sha256_ctx *c) {
    c->bitlen = 0;
    c->buflen = 0;
    c->state[0] = 0x6a09e667;
    c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372;
    c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f;
    c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab;
    c->state[7] = 0x5be0cd19;
}

void cbm_sha256_update(cbm_sha256_ctx *c, const void *data, size_t len) {
    if (len == 0) {
        return; /* data may be NULL when len is 0 */
    }
    const auto *p = static_cast<const uint8_t *>(data);
    if (c->buflen > 0) {
        size_t take = SHA256_BLOCK_BYTES - c->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen < SHA256_BLOCK_BYTES) {
            return;
        }
        sha256_blocks(c, c->buf, 1);
        c->bitlen += 512;
        c->buflen = 0;
    }
    /* Whole blocks straight from the input, without staging them in buf. */
    size_t blocks = len / SHA256_BLOCK_BYTES;
    if (blocks > 0) {
        sha256_blocks(c, p, blocks);
        c->bitlen += static_cast<uint64_t>(blocks) * 512;
        p += blocks * SHA256_BLOCK_BYTES;
        len -= blocks * SHA256_BLOCK_BYTES;
    }
    if (len > 0) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void cbm_sha256_final(cbm_sha256_ctx *c, uint8_t out[CBM_SHA256_DIGEST_LEN]) {
    c->bitlen += (uint64_t)c->buflen * 8;

    size_t i = c->buflen;
    c->buf[i++] = 0x80; /* append the '1' bit + zero padding */
    if (i > 56) {
        while (i < 64) {
            c->buf[i++] = 0;
        }
        sha256_blocks(c, c->buf, 1);
        i = 0;
    }
    while (i < 56) {
        c->buf[i++] = 0;
    }
    /* append the 64-bit big-endian message length */
    for (int j = 0; j < 8; j++) {
        c->buf[56 + j] = (uint8_t)(c->bitlen >> (56 - 8 * j));
    }
    sha256_blocks(c, c->buf, 1);

    for (int j = 0; j < 8; j++) {
        out[j * 4] = (uint8_t)(c->state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(c->state[j]);
    }
}

void cbm_sha256_hex(const void *data, size_t len, char out[CBM_SHA256_HEX_LEN + 1]) {
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_ctx c;
    cbm_sha256_init(&c);
    cbm_sha256_update(&c, data, len);
    cbm_sha256_final(&c, digest);

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}
