#ifndef CBM_MCP_HTTP_TRANSPORT_H
#define CBM_MCP_HTTP_TRANSPORT_H

#include "mcp/mcp.h"

#include <stdbool.h>

#define CBM_MCP_HTTP_DEFAULT_PORT 9766
#define CBM_MCP_HTTP_DEFAULT_SESSION_TTL_SEC 1800
#define CBM_MCP_HTTP_DEFAULT_MAX_SESSIONS 64
#define CBM_MCP_HTTP_DEFAULT_MAX_WORKERS 16

typedef struct {
    const char *bind_addr;
    int port;
    const char *auth_token;
    const char *auth_store_path;
    const char *audit_log_path;
    const char *trusted_proxies;
    int session_ttl_sec;
    int max_sessions;
    int max_workers;
    cbm_mcp_tool_profile_t tool_profile;
} cbm_mcp_http_config_t;

typedef struct cbm_mcp_http_server cbm_mcp_http_server_t;

/* Creates and binds a Streamable HTTP MCP server. The token is copied and
 * must contain at least 32 bytes. The audit log is opened during construction
 * so startup fails closed when operations cannot be recorded. */
cbm_mcp_http_server_t *cbm_mcp_http_server_new(const cbm_mcp_http_config_t *config);

void cbm_mcp_http_server_free(cbm_mcp_http_server_t *server);
void cbm_mcp_http_server_stop(cbm_mcp_http_server_t *server);
int cbm_mcp_http_server_run(cbm_mcp_http_server_t *server);
int cbm_mcp_http_server_port(const cbm_mcp_http_server_t *server);

/* Pure helpers exposed for security-focused unit tests. */
bool cbm_mcp_http_authorize(const char *authorization, const char *expected_token);
bool cbm_mcp_http_resolve_client_ip(const char *peer_ip, const char *x_forwarded_for,
                                    const char *trusted_proxies, char *out, size_t outsz);

#endif /* CBM_MCP_HTTP_TRANSPORT_H */
