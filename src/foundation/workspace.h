#ifndef CBM_FOUNDATION_WORKSPACE_H
#define CBM_FOUNDATION_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CBM_WS_ALLOW = 0,
    CBM_WS_DENY_TOO_SHALLOW,
    CBM_WS_DENY_ABSOLUTE,
    CBM_WS_DENY_SENSITIVE,
} cbm_ws_verdict_t;

/* canonical_path must be an absolute, canonicalized directory path. */
cbm_ws_verdict_t cbm_workspace_classify_root(const char *canonical_path, const char *home_dir,
                                             const char *cache_dir);
const char *cbm_workspace_verdict_reason(cbm_ws_verdict_t verdict);
bool cbm_workspace_verdict_is_overridable(cbm_ws_verdict_t verdict);
int cbm_workspace_path_depth(const char *canonical_path);

/* Apply configured containment, when present, and the always-on breadth policy. */
bool cbm_workspace_root_allowed(const char *canonical_path, const char *home_dir,
                                const char *cache_dir, const char *configured_root, char *err,
                                size_t err_sz);

const char *cbm_workspace_home_dir(void);
const char *cbm_workspace_cache_dir(void);

/* Implemented in mcp.c and shared by all indexing entry points. */
bool cbm_path_within_root(const char *root_path, const char *abs_path);

#endif
