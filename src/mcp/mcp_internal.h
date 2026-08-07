#ifndef CBM_MCP_INTERNAL_H
#define CBM_MCP_INTERNAL_H

#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "store/store.h"

/* Closed public vocabulary used by trace_path(include_evidence=true). */
const char *cbm_mcp_edge_strategy_class(const char *strategy);

/* Line-overlap primitive used by detect_changes hunk scoping. */
bool cbm_detect_node_in_hunks(const cbm_node_t *node, const cbm_changed_hunk_t *hunks,
                              int hunk_count, const char *file);

#endif
