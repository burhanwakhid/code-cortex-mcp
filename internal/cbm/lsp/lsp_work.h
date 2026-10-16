/*
 * lsp_work.h — deterministic work counter for LSP complexity tests (#1527).
 *
 * Seam builds only (CBM_ENABLE_TEST_SEAMS): every name comparison a resolver
 * primitive performs while searching (scope frame probes, registry chain
 * probes, real expression evaluations) adds to a thread-local counter, so a
 * test can assert that the work grows linearly with input size without
 * reading a clock. Product builds compile CBM_LSP_WORK to nothing.
 */
#ifndef CBM_LSP_WORK_H
#define CBM_LSP_WORK_H

#include <stdint.h>

#ifdef CBM_ENABLE_TEST_SEAMS
#ifdef __cplusplus
extern "C" {
#endif
extern thread_local uint64_t cbm_lsp_work_steps;
/* Read and reset the calling thread's counter. */
uint64_t cbm_lsp_work_take(void);
#ifdef __cplusplus
}
#endif
#define CBM_LSP_WORK(n) (cbm_lsp_work_steps += (uint64_t)(n))
#else
#define CBM_LSP_WORK(n) ((void)0)
#endif

#endif /* CBM_LSP_WORK_H */
