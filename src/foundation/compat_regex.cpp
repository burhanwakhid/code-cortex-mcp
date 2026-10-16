/*
 * compat_regex.cpp — Portable regular expression implementation.
 *
 * Backed by the system POSIX regex engine where available and std::regex on
 * Windows.  libstdc++'s std::regex executor recursively backtracks once per
 * input byte for patterns such as `.*`; sufficiently long indexed qualified
 * names can therefore overflow the process stack.  POSIX regexec is iterative
 * for this workload.  The Windows fallback rejects oversized non-trivial
 * inputs instead of risking a process crash.
 *
 * Both backends expand bounded repetition at compile time, so cbm_regcomp
 * sizes a pattern first (the compile-size guard below) and refuses one whose
 * compiled form would be too large, before the platform compiler allocates
 * anything.
 *
 * The opaque cbm_regex_t buffer stores regex_t directly on POSIX and a
 * heap-allocated std::regex state pointer on Windows. The C API is unchanged.
 */
#include "foundation/compat_regex.h"
#include "foundation/constants.h"

#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <regex>
#include <string>
#else
#include <regex.h>
#endif

namespace {

#ifdef _WIN32
struct RegexState {
    std::string pattern;
    std::regex compiled;

    RegexState(const char *source, std::regex::flag_type flags)
        : pattern(source), compiled(source, flags) {}
};

std::regex::flag_type translate_flags(int flags) {
    /* POSIX grammar: EXTENDED -> ERE, otherwise BRE. Callers use ERE. */
    std::regex::flag_type f = (flags & CBM_REG_EXTENDED) ? std::regex::extended : std::regex::basic;
    if (flags & CBM_REG_ICASE) {
        f |= std::regex::icase;
    }
    if (flags & CBM_REG_NOSUB) {
        f |= std::regex::nosubs;
    }
    if (flags & CBM_REG_NEWLINE) {
        f |= std::regex::multiline;
    }
    return f;
}
#else
int translate_flags(int flags) {
    int f = 0;
    if (flags & CBM_REG_EXTENDED) {
        f |= REG_EXTENDED;
    }
    if (flags & CBM_REG_ICASE) {
        f |= REG_ICASE;
    }
    if (flags & CBM_REG_NOSUB) {
        f |= REG_NOSUB;
    }
    if (flags & CBM_REG_NEWLINE) {
        f |= REG_NEWLINE;
    }
#ifdef REG_ENHANCED
    if (flags & CBM_REG_GREP) {
        f |= REG_ENHANCED;
    }
#endif
    return f;
}
#endif

} // namespace

#ifdef _WIN32
static_assert(sizeof(RegexState *) <= CBM_SZ_256,
              "cbm_regex_t opaque buffer too small for pointer");
#else
static_assert(sizeof(regex_t) <= CBM_SZ_256,
              "cbm_regex_t opaque buffer too small for regex_t");
#endif

/* ── Compile-size guard (shared by both backends) ─────────────────
 *
 * The platform compilers expand `X{m,n}` into n copies of X, so nested
 * intervals multiply: `((((a){20}){20}){20}){20}` is 160,000 expanded atoms
 * from 25 bytes of pattern. A K-way alternation additionally costs about K^2
 * position-set entries on the TRE-derived backend (macOS libc), because every
 * union node copies its branches' sets. glibc and std::regex expand intervals
 * the same way.
 *
 * Measured upstream on macOS libc with REG_EXTENDED|REG_NOSUB: the nested
 * shape costs about 0.6 KB of heap per expanded atom, 1.1 KB with submatch
 * tracking on, 1.7 KB with REG_ICASE and 2.3 KB with both; a K-way alternation
 * adds about 70 B x K^2 on top of its branches (twice that under REG_ICASE),
 * and an alternation under an interval pays that for every copy. glibc costs
 * about 0.45 KB per atom.
 *
 * cbm_regcomp_estimate_units() counts expanded atoms in one linear pass with
 * saturating arithmetic and charges a K-way alternation K^2/8 units, the
 * measured ratio between the two costs. With CBM_REGEX_COMPILE_BUDGET_UNITS =
 * 32768 the heaviest accepted compile is a repeated alternation at about 30 MB
 * with REG_NOSUB; the nested shape tops out at 13^4 = 28,561 units, about
 * 16 MB. `(\w{1,100}){1,20}` (2,000 units) and an alternation of 500 short
 * names (31,250 units plus its letters) remain accepted. */

namespace {

enum { REGEX_GROUP_DEPTH_MAX = 64 };

struct RegexFrame {
    uint64_t total;    /* units of this group so far; includes `last` */
    uint64_t last;     /* units of the element a following quantifier applies to */
    uint64_t branches; /* alternatives in this group: 1 + the number of '|' */
};

uint64_t regex_sat_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

uint64_t regex_sat_mul(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

void regex_frame_atom(RegexFrame *f, uint64_t units) {
    f->total = regex_sat_add(f->total, units);
    f->last = units;
}

/* An interval applies to the last element: n copies of it replace the one. */
void regex_frame_repeat_last(RegexFrame *f, uint64_t copies) {
    uint64_t grown = regex_sat_mul(f->last, copies);
    f->total = regex_sat_add(f->total - f->last, grown);
    f->last = grown;
}

/* Units a closed group contributes to its parent: its content plus the K^2/8
 * alternation share, never less than one atom. */
uint64_t regex_frame_units(const RegexFrame *f) {
    uint64_t alternation = regex_sat_mul(f->branches, f->branches) / 8;
    uint64_t units = regex_sat_add(f->total, alternation);
    return units ? units : 1;
}

/* Skip the bracket expression opening at p[i] == '['. Returns the index just
 * past its closing ']', or the end of the string when it is unterminated.
 * Nothing inside is an operator: `[]a]`, `[^]a]` and `[[:alpha:]]` are each
 * one atom, and a brace inside brackets never opens an interval. */
size_t regex_skip_bracket(const char *p, size_t i) {
    size_t j = i + 1;
    if (p[j] == '^') {
        j++;
    }
    if (p[j] == ']') {
        j++;
    }
    while (p[j] && p[j] != ']') {
        if (p[j] == '[' && (p[j + 1] == ':' || p[j + 1] == '=' || p[j + 1] == '.')) {
            char close = p[j + 1];
            size_t k = j + 2;
            while (p[k] && !(p[k] == close && p[k + 1] == ']')) {
                k++;
            }
            j = p[k] ? k + 2 : k;
        } else {
            j++;
        }
    }
    return p[j] ? j + 1 : j;
}

bool regex_parse_digits(const char *p, size_t *i, uint64_t *value) {
    bool any = false;
    *value = 0;
    while (p[*i] >= '0' && p[*i] <= '9') {
        *value = regex_sat_add(regex_sat_mul(*value, 10), static_cast<uint64_t>(p[*i] - '0'));
        (*i)++;
        any = true;
    }
    return any;
}

/* Parse the interval body that starts at p[i], just past the opening brace
 * (`{` in extended syntax, `\{` in basic). On success stores the number of
 * copies the compiler will make (the upper bound, or the lower bound for
 * `{m,}`, at least 1) and the index just past the closing brace. Anything that
 * is not an interval leaves the brace to be counted as a literal, which is what
 * the backends that accept it do; the others reject the pattern. */
bool regex_parse_interval(const char *p, size_t i, bool extended, uint64_t *copies, size_t *end) {
    uint64_t lower = 0;
    uint64_t upper = 0;
    bool has_lower = regex_parse_digits(p, &i, &lower);
    bool has_upper = false;
    if (p[i] == ',') {
        i++;
        has_upper = regex_parse_digits(p, &i, &upper);
    } else {
        if (!has_lower) {
            return false;
        }
        upper = lower;
        has_upper = true;
    }
    if (extended) {
        if (p[i] != '}') {
            return false;
        }
        i++;
    } else {
        if (p[i] != '\\' || p[i + 1] != '}') {
            return false;
        }
        i += 2;
    }
    uint64_t n = has_upper ? upper : lower;
    if (has_upper && lower > upper) {
        n = lower; /* `{5,3}` is rejected by the backends; count the larger bound */
    }
    *copies = n ? n : 1;
    *end = i;
    return true;
}

/* Refuse a pattern before the platform compiler sees it. */
int regex_compile_guard(const char *pattern, int flags) {
    size_t len = 0;
    while (len <= static_cast<size_t>(CBM_REGEX_PATTERN_MAX_BYTES) && pattern[len]) {
        len++;
    }
    if (len > static_cast<size_t>(CBM_REGEX_PATTERN_MAX_BYTES)) {
        return CBM_REG_ETOOBIG;
    }
    if (cbm_regcomp_estimate_units(pattern, flags) >
        static_cast<uint64_t>(CBM_REGEX_COMPILE_BUDGET_UNITS)) {
        return CBM_REG_ETOOBIG;
    }
    return CBM_REG_OK;
}

} // namespace

uint64_t cbm_regcomp_estimate_units(const char *pattern, int flags) {
    const bool extended = (flags & CBM_REG_EXTENDED) != 0;
    RegexFrame frames[REGEX_GROUP_DEPTH_MAX];
    int depth = 0;
    frames[0] = RegexFrame{0, 0, 1};

    size_t i = 0;
    while (pattern[i]) {
        char c = pattern[i];
        if (c == '[') {
            i = regex_skip_bracket(pattern, i);
            regex_frame_atom(&frames[depth], 1);
            continue;
        }
        /* In extended syntax the bare characters operate and `\x` is one
         * literal; in basic syntax only `\(`, `\)`, `\{`, `\|`, `\+`, `\?` and
         * a bare `*` operate. */
        char op = 0;
        size_t len = 1;
        if (c == '\\') {
            len = pattern[i + 1] ? 2 : 1;
            if (!extended && len == 2 && std::strchr("(){|+?", pattern[i + 1])) {
                op = pattern[i + 1];
            }
        } else if (extended ? std::strchr("(){|*+?", c) != nullptr : c == '*') {
            op = c;
        }
        RegexFrame *f = &frames[depth];
        switch (op) {
        case '(':
            if (depth + 1 >= REGEX_GROUP_DEPTH_MAX) {
                return UINT64_MAX;
            }
            frames[++depth] = RegexFrame{0, 0, 1};
            break;
        case ')':
            if (depth == 0) {
                regex_frame_atom(f, 1); /* unmatched: a literal where accepted */
                break;
            }
            depth--;
            regex_frame_atom(&frames[depth], regex_frame_units(f));
            break;
        case '|':
            f->branches = regex_sat_add(f->branches, 1);
            f->last = 0;
            break;
        case '{': {
            uint64_t copies = 0;
            size_t end = 0;
            if (regex_parse_interval(pattern, i + len, extended, &copies, &end)) {
                regex_frame_repeat_last(f, copies);
                i = end;
                continue;
            }
            regex_frame_atom(f, 1);
            break;
        }
        case '*':
        case '+':
        case '?':
            break; /* unbounded or optional repetition is not expanded */
        default:
            regex_frame_atom(f, 1);
            break;
        }
        i += len;
    }
    while (depth > 0) {
        RegexFrame *f = &frames[depth];
        depth--;
        regex_frame_atom(&frames[depth], regex_frame_units(f));
    }
    return regex_frame_units(&frames[0]);
}

int cbm_regex_too_big(const char *pattern, int flags) {
    return pattern && regex_compile_guard(pattern, flags) == CBM_REG_ETOOBIG;
}

int cbm_regcomp(cbm_regex_t *r, const char *pattern, int flags) {
    int refused = regex_compile_guard(pattern, flags);
    if (refused != CBM_REG_OK) {
        return refused;
    }
#ifdef _WIN32
    try {
        RegexState *re = new RegexState(pattern, translate_flags(flags));
        std::memcpy(r->opaque, &re, sizeof(re));
        return CBM_REG_OK;
    } catch (...) {
        /* Invalid pattern (std::regex_error) or OOM: non-zero = compile error,
         * matching the prior regcomp contract (callers only check != 0). */
        return 1;
    }
#else
    regex_t *re = reinterpret_cast<regex_t *>(r->opaque);
    int rc = regcomp(re, pattern, translate_flags(flags));
    return rc == 0 ? CBM_REG_OK : rc;
#endif
}

int cbm_regexec(const cbm_regex_t *r, const char *str, int nmatch, cbm_regmatch_t *matches,
                int eflags) {
#ifdef _WIN32
    (void)eflags; /* All call sites pass 0; POSIX exec flags are unused. */
    RegexState *re = nullptr;
    std::memcpy(&re, r->opaque, sizeof(re));
    if (!re) {
        return CBM_REG_NOMATCH;
    }
    if (re->pattern == ".*") {
        if (nmatch > 0 && matches) {
            matches[0].rm_so = 0;
            matches[0].rm_eo = static_cast<int>(std::strlen(str));
            for (int i = 1; i < nmatch && i < CBM_SZ_32; ++i) {
                matches[i].rm_so = -1;
                matches[i].rm_eo = -1;
            }
        }
        return CBM_REG_OK;
    }
    constexpr size_t kMaxSafeStdRegexInput = 64U * 1024U;
    if (std::strlen(str) > kMaxSafeStdRegexInput) {
        return CBM_REG_NOMATCH;
    }
    try {
        if (nmatch <= 0 || !matches) {
            return std::regex_search(str, re->compiled) ? CBM_REG_OK : CBM_REG_NOMATCH;
        }
        std::cmatch m;
        if (!std::regex_search(str, m, re->compiled)) {
            return CBM_REG_NOMATCH;
        }
        int n = nmatch > CBM_SZ_32 ? CBM_SZ_32 : nmatch;
        for (int i = 0; i < n; i++) {
            if (static_cast<size_t>(i) < m.size() && m[i].matched) {
                matches[i].rm_so = static_cast<int>(m.position(static_cast<size_t>(i)));
                matches[i].rm_eo = static_cast<int>(m.position(static_cast<size_t>(i)) +
                                                    m.length(static_cast<size_t>(i)));
            } else {
                matches[i].rm_so = -1;
                matches[i].rm_eo = -1;
            }
        }
        return CBM_REG_OK;
    } catch (...) {
        /* Pathological backtracking / runtime error -> treat as no match. */
        return CBM_REG_NOMATCH;
    }
#else
    const regex_t *re = reinterpret_cast<const regex_t *>(r->opaque);
    if (nmatch <= 0 || !matches) {
        int rc = regexec(re, str, 0, nullptr, eflags);
        return rc == 0 ? CBM_REG_OK : CBM_REG_NOMATCH;
    }
    regmatch_t native_matches[CBM_SZ_32];
    int n = nmatch > CBM_SZ_32 ? CBM_SZ_32 : nmatch;
    int rc = regexec(re, str, static_cast<size_t>(n), native_matches, eflags);
    if (rc != 0) {
        return CBM_REG_NOMATCH;
    }
    for (int i = 0; i < n; ++i) {
        matches[i].rm_so = static_cast<int>(native_matches[i].rm_so);
        matches[i].rm_eo = static_cast<int>(native_matches[i].rm_eo);
    }
    return CBM_REG_OK;
#endif
}

void cbm_regfree(cbm_regex_t *r) {
#ifdef _WIN32
    RegexState *re = nullptr;
    std::memcpy(&re, r->opaque, sizeof(re));
    delete re;
    std::memset(r->opaque, 0,
                sizeof(RegexState *)); /* clear stale ptr (cppcheck: avoid re after delete) */
#else
    regex_t *re = reinterpret_cast<regex_t *>(r->opaque);
    regfree(re);
#endif
}
