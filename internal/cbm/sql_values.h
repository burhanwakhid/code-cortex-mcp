/* sql_values.h — keep a SQL data dump's literal rows out of the parse (#1735).
 *
 * A mysqldump-style file is almost entirely `INSERT ... VALUES (..),(..),...`
 * rows of plain literals. Those rows contribute nothing to the graph (no
 * definition, call or usage lives in a literal), yet tree-sitter builds a full
 * tree for every one of them: ~70 bytes of tree per source byte and seconds of
 * parse per ten megabytes, until the per-file budget gives up and the whole file
 * — its CREATE TABLE statements included — is skipped as "parse timeout".
 *
 * cbm_sql_values_kept_ranges() finds, in one linear quote- and comment-aware
 * scan, every value tuple AFTER THE FIRST of an INSERT/REPLACE ... VALUES list
 * that consists only of literals (strings, numbers, NULL, TRUE, FALSE, DEFAULT),
 * and returns the complement as ranges of kept text. cbm_sql_blank_literal_rows()
 * turns that into a parse copy of the source with every excluded byte blanked to
 * a space (newlines kept), the same idiom as the C/C++ statement-macro blanking:
 * the parser then sees `INSERT INTO t VALUES (first row)    ;` — a complete
 * statement — while every byte offset and line/column of the kept text is
 * unchanged, so every node the extractors read still points at the real source.
 *
 * A syntax rule, not a work cap: the same file always yields the same ranges,
 * whatever its size. A tuple holding anything that could reference code — a
 * subquery, a function call, an identifier, a quoted identifier — is kept.
 *
 * Ported from DeusData/codebase-memory-mcp@c0a06b3d (upstream feeds the ranges
 * to ts_parser_set_included_ranges; the fork blanks instead). */
#ifndef CBM_SQL_VALUES_H
#define CBM_SQL_VALUES_H

#include "tree_sitter/api.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    TSRange *items; /* ascending, non-overlapping; owned (cbm_sql_kept_ranges_free) */
    uint32_t count;
} CBMSqlKeptRanges;

/* Compute the kept ranges for `src`. Returns true and fills `out` only when at
 * least one literal tuple was excluded; returns false (out zeroed) when the
 * whole file must be parsed as-is or on allocation failure. */
bool cbm_sql_values_kept_ranges(const char *src, uint32_t len, CBMSqlKeptRanges *out);

void cbm_sql_kept_ranges_free(CBMSqlKeptRanges *ranges);

/* A heap copy of `src` (len bytes, NUL-terminated) with every byte outside the
 * kept ranges blanked to ' ' (line breaks preserved), or NULL when nothing is
 * excluded or on allocation failure — then parse `src` itself. Caller frees. */
char *cbm_sql_blank_literal_rows(const char *src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CBM_SQL_VALUES_H */
