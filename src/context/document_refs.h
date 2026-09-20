#ifndef CBM_DOCUMENT_REFS_H
#define CBM_DOCUMENT_REFS_H

#include "store/store.h"

/* Returns malloc-owned JSON, or NULL on invalid input/store/allocation failure.
 * Only indexed document_links REFERENCES are included. Targets are exact node IDs;
 * no containing-file or symbol-name expansion is performed. Limit is clamped to
 * 0..100; zero returns counts and analysis status without evidence rows.
 * Counts cover all matching edges, while retained evidence is bounded by limit.
 * Status describes the supported parser subset, not complete Markdown coverage. */
char *cbm_document_refs_json(cbm_store_t *store, const char *project, const cbm_node_t *targets,
                             int target_count, int limit);

/* Indexed coverage only, not proof of missing documentation. Sorted by file path.
 * view is code or documents; status is NULL/empty or a status for that view.
 * offset is nonnegative; limit is clamped to 0..100. Returns malloc-owned JSON. */
char *cbm_document_coverage_json(cbm_store_t *store, const char *project, const char *view,
                                 const char *status, int offset, int limit);

#endif
