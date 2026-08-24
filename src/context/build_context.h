#ifndef CBM_BUILD_CONTEXT_H
#define CBM_BUILD_CONTEXT_H

#include "store/store.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *project;
    const char *task;
    const char *target;
    const char *diff_ref;
    const char *evidence_level; /* "scout", "analysis", or "audit" */
    int token_budget;
    bool include_docs;
    bool include_tests;
} cbm_context_request_t;

typedef struct {
    int returned;
    int total;
    int estimated_tokens;
    bool truncated;
    bool budget_truncated;
    bool resolution_incomplete;
} cbm_context_stats_t;

/* Build the deterministic context payload. The returned JSON object is heap
 * allocated and must be freed by the caller. Analysis metadata is attached by
 * the MCP handler because it owns the live source/coverage context. */
char *cbm_context_build_json(cbm_store_t *store, const cbm_context_request_t *request,
                             cbm_context_stats_t *stats);

#endif /* CBM_BUILD_CONTEXT_H */
