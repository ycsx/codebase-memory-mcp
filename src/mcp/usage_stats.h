/* Persistent, privacy-minimal MCP tool usage statistics. */
#ifndef CBM_USAGE_STATS_H
#define CBM_USAGE_STATS_H

#include <stdbool.h>
#include <stdint.h>

struct cbm_store;
typedef struct cbm_store cbm_store_t;

typedef struct cbm_usage_store cbm_usage_store_t;

/* Opens the shared usage database under the configured cache directory. */
cbm_usage_store_t *cbm_usage_store_open(void);
void cbm_usage_store_close(cbm_usage_store_t *store);

/* Records one initialized MCP client's completed tools/call request.
 * Arguments are inspected only to derive project and target-file attribution;
 * query text, source, and result payloads are never persisted. */
void cbm_usage_record(cbm_usage_store_t *store, const char *tool_name, const char *args_json,
                      cbm_store_t *graph_store, const char *current_project, bool is_error,
                      int64_t duration_us);

/* Returns heap-allocated JSON for the desktop/console usage panel. */
char *cbm_usage_stats_json(int file_limit);

#endif /* CBM_USAGE_STATS_H */
