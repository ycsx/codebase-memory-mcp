#include "mcp/http_transport.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/log.h"
#include "foundation/sha256.h"
#include "ui/httpd.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/stat.h>
#endif

enum {
    TOKEN_MIN_LEN = 32,
    TOKEN_MAX_LEN = 512,
    SESSION_ID_HEX_LEN = 64,
    REQUEST_ID_HEX_LEN = 16,
    CLIENT_IP_LEN = 64,
    AUDIT_VALUE_LEN = 512,
    ACCEPT_TIMEOUT_MS = 100,
    SHUTDOWN_POLL_US = 10000,
};

typedef struct {
    bool used;
    char id[SESSION_ID_HEX_LEN + 1];
    char client_ip[CLIENT_IP_LEN];
    time_t last_used;
    int in_flight;
    cbm_mcp_server_t *mcp;
    cbm_mutex_t mutex;
} cbm_mcp_http_session_t;

struct cbm_mcp_http_server {
    cbm_httpd_t *httpd;
    char *auth_token;
    char token_key_id[13];
    char *audit_log_path;
    char *trusted_proxies;
    int session_ttl_sec;
    int max_sessions;
    int max_workers;
    cbm_mcp_tool_profile_t tool_profile;
    cbm_mcp_http_session_t *sessions;
    cbm_mutex_t sessions_mutex;
    cbm_mutex_t audit_mutex;
    atomic_int stop;
    atomic_int active_workers;
    atomic_uint_fast64_t nonce;
};

typedef struct {
    cbm_mcp_http_server_t *server;
    cbm_http_conn_t *conn;
} cbm_mcp_http_worker_t;

static char *string_dup(const char *value) {
    if (!value)
        return NULL;
    size_t len = strlen(value);
    char *copy = malloc(len + 1);
    if (copy)
        memcpy(copy, value, len + 1);
    return copy;
}

static bool copy_string(char *out, size_t outsz, const char *value, size_t len) {
    if (!out || outsz == 0 || !value || len >= outsz)
        return false;
    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

static bool constant_time_equal(const char *left, const char *right) {
    if (!left || !right)
        return false;
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t max_len = left_len > right_len ? left_len : right_len;
    unsigned int diff = (unsigned int)(left_len ^ right_len);
    for (size_t i = 0; i < max_len; i++) {
        unsigned char a = i < left_len ? (unsigned char)left[i] : 0U;
        unsigned char b = i < right_len ? (unsigned char)right[i] : 0U;
        diff |= (unsigned int)(a ^ b);
    }
    return diff == 0U;
}

bool cbm_mcp_http_authorize(const char *authorization, const char *expected_token) {
    if (!authorization || !expected_token || strncmp(authorization, "Bearer ", 7) != 0)
        return false;
    const char *presented = authorization + 7;
    size_t len = strlen(presented);
    if (len == 0 || len > TOKEN_MAX_LEN)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)presented[i];
        if (ch <= 0x20U || ch >= 0x7fU)
            return false;
    }
    return constant_time_equal(presented, expected_token);
}

static bool parse_ipv4(const char *value, uint32_t *out) {
    struct in_addr addr;
    if (!value || !out || inet_pton(AF_INET, value, &addr) != 1)
        return false;
    *out = ntohl(addr.s_addr);
    return true;
}

static bool ipv4_matches_entry(const char *ip, const char *entry, size_t entry_len) {
    char item[64];
    if (!copy_string(item, sizeof(item), entry, entry_len))
        return false;
    char *slash = strchr(item, '/');
    int prefix = 32;
    if (slash) {
        *slash++ = '\0';
        if (*slash == '\0')
            return false;
        char *end = NULL;
        long parsed = strtol(slash, &end, 10);
        if (!end || *end != '\0' || parsed < 0 || parsed > 32)
            return false;
        prefix = (int)parsed;
    }
    uint32_t candidate = 0, network = 0;
    if (!parse_ipv4(ip, &candidate) || !parse_ipv4(item, &network))
        return false;
    uint32_t mask = prefix == 0 ? 0U : UINT32_MAX << (32 - prefix);
    return (candidate & mask) == (network & mask);
}

static bool ip_is_trusted(const char *ip, const char *trusted_proxies) {
    if (!ip || !trusted_proxies)
        return false;
    const char *p = trusted_proxies;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p))
            p++;
        const char *end = p;
        while (*end && *end != ',')
            end++;
        const char *trimmed = end;
        while (trimmed > p && isspace((unsigned char)trimmed[-1]))
            trimmed--;
        if (trimmed > p && ipv4_matches_entry(ip, p, (size_t)(trimmed - p)))
            return true;
        p = end;
    }
    return false;
}

bool cbm_mcp_http_resolve_client_ip(const char *peer_ip, const char *x_forwarded_for,
                                    const char *trusted_proxies, char *out, size_t outsz) {
    if (!peer_ip || !out || outsz == 0)
        return false;
    const char *selected = peer_ip;
    size_t selected_len = strlen(peer_ip);
    if (x_forwarded_for && x_forwarded_for[0] && ip_is_trusted(peer_ip, trusted_proxies)) {
        const char *start = x_forwarded_for;
        while (isspace((unsigned char)*start))
            start++;
        const char *end = strchr(start, ',');
        if (!end)
            end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        char forwarded[CLIENT_IP_LEN];
        uint32_t ignored = 0;
        if (copy_string(forwarded, sizeof(forwarded), start, (size_t)(end - start)) &&
            parse_ipv4(forwarded, &ignored)) {
            selected = start;
            selected_len = (size_t)(end - start);
        }
    }
    return copy_string(out, outsz, selected, selected_len);
}

static void make_nonce(cbm_mcp_http_server_t *server, const char *kind, const char *client_ip,
                       char out[CBM_SHA256_HEX_LEN + 1]) {
    uint64_t counter = atomic_fetch_add(&server->nonce, 1U) + 1U;
    time_t now = time(NULL);
    cbm_sha256_ctx ctx;
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, server->auth_token, strlen(server->auth_token));
    cbm_sha256_update(&ctx, &counter, sizeof(counter));
    cbm_sha256_update(&ctx, &now, sizeof(now));
    cbm_sha256_update(&ctx, kind, strlen(kind));
    cbm_sha256_update(&ctx, client_ip, strlen(client_ip));
    cbm_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}

static void audit_json_string(FILE *file, const char *value) {
    fputc('"', file);
    if (value) {
        for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', file);
                fputc((int)*p, file);
            } else if (*p == '\n') {
                fputs("\\n", file);
            } else if (*p == '\r') {
                fputs("\\r", file);
            } else if (*p == '\t') {
                fputs("\\t", file);
            } else if (*p < 0x20U) {
                fprintf(file, "\\u%04x", (unsigned int)*p);
            } else {
                fputc((int)*p, file);
            }
        }
    }
    fputc('"', file);
}

static void iso8601_now(char out[32]) {
    time_t now = time(NULL);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static FILE *audit_open(const char *path) {
    FILE *file = cbm_fopen(path, "ab");
#ifndef _WIN32
    if (file)
        (void)chmod(path, S_IRUSR | S_IWUSR);
#endif
    return file;
}

static void write_audit(cbm_mcp_http_server_t *server, const char *request_id,
                        const char *client_ip, const char *session_id, const char *method,
                        const char *tool, const char *target, const char *status, int http_status,
                        int64_t duration_ms, size_t request_bytes, size_t response_bytes) {
    char timestamp[32];
    iso8601_now(timestamp);
    cbm_mutex_lock(&server->audit_mutex);
    FILE *file = audit_open(server->audit_log_path);
    if (file) {
        fputs("{\"ts\":", file);
        audit_json_string(file, timestamp);
        fputs(",\"request_id\":", file);
        audit_json_string(file, request_id);
        fputs(",\"client_ip\":", file);
        audit_json_string(file, client_ip);
        fputs(",\"principal\":", file);
        char principal[CLIENT_IP_LEN + 4];
        snprintf(principal, sizeof(principal), "ip:%s", client_ip ? client_ip : "unknown");
        audit_json_string(file, principal);
        fputs(",\"auth_key_id\":", file);
        audit_json_string(file, server->token_key_id);
        fputs(",\"session_id\":", file);
        audit_json_string(file, session_id ? session_id : "");
        fputs(",\"method\":", file);
        audit_json_string(file, method ? method : "");
        fputs(",\"tool\":", file);
        audit_json_string(file, tool ? tool : "");
        fputs(",\"target\":", file);
        audit_json_string(file, target ? target : "");
        fputs(",\"status\":", file);
        audit_json_string(file, status ? status : "error");
        fprintf(file,
                ",\"http_status\":%d,\"duration_ms\":%lld,\"request_bytes\":%zu,"
                "\"response_bytes\":%zu}\n",
                http_status, (long long)duration_ms, request_bytes, response_bytes);
        fclose(file);
    } else {
        cbm_log_error("mcp_http.audit.write_failed", "path", server->audit_log_path);
    }
    cbm_mutex_unlock(&server->audit_mutex);
}

static bool content_type_is_json(const char *content_type) {
    static const char expected[] = "application/json";
    if (!content_type)
        return false;
    for (size_t i = 0; i < sizeof(expected) - 1; i++) {
        if (tolower((unsigned char)content_type[i]) != expected[i])
            return false;
    }
    char suffix = content_type[sizeof(expected) - 1];
    return suffix == '\0' || suffix == ';' || isspace((unsigned char)suffix);
}

static void reply_json(cbm_http_conn_t *conn, int status, const char *session_id,
                       const char *body) {
    char headers[512];
    int written;
    if (session_id && session_id[0]) {
        written = snprintf(headers, sizeof(headers),
                           "Content-Type: application/json\r\nCache-Control: no-store\r\n"
                           "Mcp-Session-Id: %s\r\n",
                           session_id);
    } else {
        written = snprintf(headers, sizeof(headers),
                           "Content-Type: application/json\r\nCache-Control: no-store\r\n");
    }
    if (written < 0 || written >= (int)sizeof(headers))
        return;
    cbm_http_replyf(conn, status, headers, "%s", body ? body : "");
}

static void reply_empty(cbm_http_conn_t *conn, int status, const char *session_id) {
    char headers[384];
    int written = snprintf(headers, sizeof(headers), "Cache-Control: no-store\r\n%s%s%s",
                           session_id && session_id[0] ? "Mcp-Session-Id: " : "",
                           session_id && session_id[0] ? session_id : "",
                           session_id && session_id[0] ? "\r\n" : "");
    if (written < 0 || written >= (int)sizeof(headers))
        return;
    cbm_http_reply_buf(conn, status, headers, "", 0);
}

static void session_destroy(cbm_mcp_http_session_t *session) {
    if (!session || !session->used)
        return;
    cbm_mcp_server_free(session->mcp);
    session->mcp = NULL;
    session->used = false;
    session->id[0] = '\0';
    session->client_ip[0] = '\0';
    session->last_used = 0;
    session->in_flight = 0;
}

static void evict_expired_sessions_locked(cbm_mcp_http_server_t *server, time_t now) {
    for (int i = 0; i < server->max_sessions; i++) {
        cbm_mcp_http_session_t *session = &server->sessions[i];
        if (session->used && session->in_flight == 0 &&
            now - session->last_used >= server->session_ttl_sec) {
            session_destroy(session);
        }
    }
}

static cbm_mcp_http_session_t *session_create(cbm_mcp_http_server_t *server,
                                              const char *client_ip) {
    cbm_mutex_lock(&server->sessions_mutex);
    evict_expired_sessions_locked(server, time(NULL));
    cbm_mcp_http_session_t *slot = NULL;
    for (int i = 0; i < server->max_sessions; i++) {
        if (!server->sessions[i].used) {
            slot = &server->sessions[i];
            break;
        }
    }
    if (!slot) {
        cbm_mutex_unlock(&server->sessions_mutex);
        return NULL;
    }
    slot->mcp = cbm_mcp_server_new(NULL);
    if (!slot->mcp) {
        cbm_mutex_unlock(&server->sessions_mutex);
        return NULL;
    }
    cbm_mcp_server_set_tool_profile(slot->mcp, server->tool_profile);
    char nonce[CBM_SHA256_HEX_LEN + 1];
    make_nonce(server, "session", client_ip, nonce);
    memcpy(slot->id, nonce, SESSION_ID_HEX_LEN + 1);
    snprintf(slot->client_ip, sizeof(slot->client_ip), "%s", client_ip);
    slot->last_used = time(NULL);
    slot->in_flight = 1;
    slot->used = true;
    cbm_mutex_unlock(&server->sessions_mutex);
    return slot;
}

static cbm_mcp_http_session_t *session_acquire(cbm_mcp_http_server_t *server,
                                               const char *session_id, const char *client_ip,
                                               bool *ip_mismatch) {
    *ip_mismatch = false;
    cbm_mutex_lock(&server->sessions_mutex);
    evict_expired_sessions_locked(server, time(NULL));
    cbm_mcp_http_session_t *found = NULL;
    for (int i = 0; i < server->max_sessions; i++) {
        cbm_mcp_http_session_t *session = &server->sessions[i];
        if (session->used && constant_time_equal(session->id, session_id)) {
            if (strcmp(session->client_ip, client_ip) != 0) {
                *ip_mismatch = true;
            } else {
                session->in_flight++;
                found = session;
            }
            break;
        }
    }
    cbm_mutex_unlock(&server->sessions_mutex);
    return found;
}

static void session_release(cbm_mcp_http_server_t *server, cbm_mcp_http_session_t *session) {
    cbm_mutex_lock(&server->sessions_mutex);
    if (session->used) {
        session->last_used = time(NULL);
        if (session->in_flight > 0)
            session->in_flight--;
    }
    cbm_mutex_unlock(&server->sessions_mutex);
}

static bool session_delete(cbm_mcp_http_server_t *server, const char *session_id,
                           const char *client_ip, bool *ip_mismatch) {
    *ip_mismatch = false;
    bool deleted = false;
    cbm_mutex_lock(&server->sessions_mutex);
    for (int i = 0; i < server->max_sessions; i++) {
        cbm_mcp_http_session_t *session = &server->sessions[i];
        if (!session->used || !constant_time_equal(session->id, session_id))
            continue;
        if (strcmp(session->client_ip, client_ip) != 0) {
            *ip_mismatch = true;
        } else if (session->in_flight == 0) {
            session_destroy(session);
            deleted = true;
        }
        break;
    }
    cbm_mutex_unlock(&server->sessions_mutex);
    return deleted;
}

static void audit_target(const cbm_jsonrpc_request_t *rpc, char *tool, size_t toolsz, char *target,
                         size_t targetsz) {
    tool[0] = '\0';
    target[0] = '\0';
    if (!rpc || !rpc->method || strcmp(rpc->method, "tools/call") != 0 || !rpc->params_raw)
        return;
    char *parsed_tool = cbm_mcp_get_tool_name(rpc->params_raw);
    char *arguments = cbm_mcp_get_arguments(rpc->params_raw);
    if (parsed_tool)
        snprintf(tool, toolsz, "%.*s", (int)toolsz - 1, parsed_tool);
    if (arguments) {
        static const char *const keys[] = {"project", "repo_path", "name", "qualified_name",
                                           "path"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            char *value = cbm_mcp_get_string_arg(arguments, keys[i]);
            if (value) {
                snprintf(target, targetsz, "%s=%.*s", keys[i], (int)targetsz - 16, value);
                free(value);
                break;
            }
        }
    }
    free(arguments);
    free(parsed_tool);
}

static int64_t elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return (int64_t)(end->tv_sec - start->tv_sec) * 1000 +
           (int64_t)(end->tv_nsec - start->tv_nsec) / 1000000;
}

static void process_connection(cbm_mcp_http_server_t *server, cbm_http_conn_t *conn) {
    struct timespec started, finished;
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    char peer_ip[CLIENT_IP_LEN] = "unknown";
    (void)cbm_http_conn_peer_ip(conn, peer_ip, sizeof(peer_ip));
    char request_nonce[CBM_SHA256_HEX_LEN + 1];
    make_nonce(server, "request", peer_ip, request_nonce);
    char request_id[REQUEST_ID_HEX_LEN + 1];
    memcpy(request_id, request_nonce, REQUEST_ID_HEX_LEN);
    request_id[REQUEST_ID_HEX_LEN] = '\0';

    cbm_http_req_t request;
    memset(&request, 0, sizeof(request));
    int read_status = cbm_httpd_read_request(conn, &request);
    char client_ip[CLIENT_IP_LEN];
    if (!cbm_mcp_http_resolve_client_ip(peer_ip, request.x_forwarded_for, server->trusted_proxies,
                                        client_ip, sizeof(client_ip))) {
        snprintf(client_ip, sizeof(client_ip), "%s", peer_ip);
    }

    char audit_method[128];
    char audit_session[128];
    snprintf(audit_method, sizeof(audit_method), "%s", request.method);
    snprintf(audit_session, sizeof(audit_session), "%s", request.mcp_session_id);
    char audit_tool[128] = "";
    char audit_target_value[AUDIT_VALUE_LEN] = "";
    const char *audit_status = "error";

    if (read_status != 0) {
        if (read_status > 0)
            cbm_http_replyf(conn, read_status, "Cache-Control: no-store\r\n", "%s", "bad request");
        goto done;
    }

    if (strcmp(request.path, "/mcp") != 0) {
        cbm_http_replyf(conn, 404, "Cache-Control: no-store\r\n", "%s", "not found");
        goto done;
    }
    if (!cbm_mcp_http_authorize(request.authorization, server->auth_token)) {
        cbm_http_replyf(conn, 401,
                        "WWW-Authenticate: Bearer realm=\"codebase-memory-mcp\"\r\n"
                        "Cache-Control: no-store\r\n",
                        "%s", "unauthorized");
        audit_status = "auth_failed";
        goto done;
    }
    if (request.origin[0] != '\0') {
        cbm_http_replyf(conn, 403, "Cache-Control: no-store\r\n", "%s", "forbidden origin");
        audit_status = "origin_rejected";
        goto done;
    }

    if (strcmp(request.method, "GET") == 0) {
        cbm_http_replyf(conn, 405, "Allow: POST, DELETE\r\nCache-Control: no-store\r\n", "%s",
                        "server-sent events are not enabled");
        audit_status = "method_not_allowed";
        goto done;
    }

    if (strcmp(request.method, "DELETE") == 0) {
        if (request.mcp_session_id[0] == '\0') {
            reply_json(conn, 400, NULL, "{\"error\":\"missing MCP session\"}");
            goto done;
        }
        bool ip_mismatch = false;
        if (!session_delete(server, request.mcp_session_id, client_ip, &ip_mismatch)) {
            reply_json(conn, ip_mismatch ? 403 : 404, NULL,
                       ip_mismatch ? "{\"error\":\"session IP mismatch\"}"
                                   : "{\"error\":\"unknown MCP session\"}");
            audit_status = ip_mismatch ? "session_ip_mismatch" : "session_not_found";
            goto done;
        }
        reply_empty(conn, 204, NULL);
        audit_status = "ok";
        goto done;
    }

    if (strcmp(request.method, "POST") != 0) {
        cbm_http_replyf(conn, 405, "Allow: POST, DELETE\r\nCache-Control: no-store\r\n", "%s",
                        "method not allowed");
        audit_status = "method_not_allowed";
        goto done;
    }
    if (!content_type_is_json(request.content_type)) {
        reply_json(conn, 415, NULL, "{\"error\":\"Content-Type must be application/json\"}");
        audit_status = "unsupported_media_type";
        goto done;
    }
    if (!request.body || request.body_len == 0) {
        reply_json(conn, 400, NULL, "{\"error\":\"empty request body\"}");
        goto done;
    }

    cbm_jsonrpc_request_t rpc;
    memset(&rpc, 0, sizeof(rpc));
    if (cbm_jsonrpc_parse(request.body, &rpc) != 0 || !rpc.method) {
        reply_json(conn, 400, NULL,
                   "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,"
                   "\"message\":\"Parse error\"},\"id\":null}");
        audit_status = "parse_error";
        goto done;
    }
    snprintf(audit_method, sizeof(audit_method), "%s", rpc.method);
    audit_target(&rpc, audit_tool, sizeof(audit_tool), audit_target_value,
                 sizeof(audit_target_value));

    bool is_initialize = strcmp(rpc.method, "initialize") == 0;
    cbm_mcp_http_session_t *session = NULL;
    bool ip_mismatch = false;
    if (request.mcp_session_id[0]) {
        session = session_acquire(server, request.mcp_session_id, client_ip, &ip_mismatch);
    } else if (is_initialize) {
        session = session_create(server, client_ip);
    }
    if (!session) {
        int status = ip_mismatch ? 403 : (is_initialize ? 503 : 404);
        const char *message = ip_mismatch     ? "session IP mismatch"
                              : is_initialize ? "session capacity reached"
                                              : "missing or unknown MCP session";
        char body[160];
        snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
        reply_json(conn, status, NULL, body);
        audit_status = ip_mismatch ? "session_ip_mismatch"
                                   : (is_initialize ? "session_capacity" : "session_not_found");
        cbm_jsonrpc_request_free(&rpc);
        goto done;
    }
    snprintf(audit_session, sizeof(audit_session), "%s", session->id);

    cbm_mutex_lock(&session->mutex);
    char *response = cbm_mcp_server_handle(session->mcp, request.body);
    cbm_mutex_unlock(&session->mutex);
    if (response) {
        reply_json(conn, 200, session->id, response);
        audit_status = strstr(response, "\"error\"") ? "error" : "ok";
        free(response);
    } else {
        reply_empty(conn, 202, session->id);
        audit_status = "ok";
    }
    session_release(server, session);
    cbm_jsonrpc_request_free(&rpc);

done:
    cbm_clock_gettime(CLOCK_MONOTONIC, &finished);
    int status = cbm_http_conn_status(conn);
    write_audit(server, request_id, client_ip, audit_session, audit_method, audit_tool,
                audit_target_value, audit_status, status, elapsed_ms(&started, &finished),
                cbm_http_conn_request_bytes(conn), cbm_http_conn_response_bytes(conn));
    cbm_http_req_free(&request);
}

static void *connection_worker(void *arg) {
    cbm_mcp_http_worker_t *worker = arg;
    process_connection(worker->server, worker->conn);
    cbm_httpd_conn_close(worker->conn);
    atomic_fetch_sub(&worker->server->active_workers, 1);
    free(worker);
    return NULL;
}

cbm_mcp_http_server_t *cbm_mcp_http_server_new(const cbm_mcp_http_config_t *config) {
    if (!config || !config->bind_addr || !config->auth_token || !config->audit_log_path ||
        strlen(config->auth_token) < TOKEN_MIN_LEN || strlen(config->auth_token) > TOKEN_MAX_LEN ||
        config->port < 0 || config->port > 65535 || config->max_sessions <= 0 ||
        config->max_sessions > 1024 || config->max_workers <= 0 || config->max_workers > 256 ||
        config->session_ttl_sec <= 0 || config->tool_profile == CBM_MCP_TOOL_PROFILE_ALL) {
        cbm_log_error("mcp_http.config.invalid", "reason", "missing_or_out_of_range");
        return NULL;
    }
    cbm_mcp_http_server_t *server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    server->auth_token = string_dup(config->auth_token);
    server->audit_log_path = string_dup(config->audit_log_path);
    server->trusted_proxies = string_dup(config->trusted_proxies ? config->trusted_proxies : "");
    server->sessions = calloc((size_t)config->max_sessions, sizeof(*server->sessions));
    if (!server->auth_token || !server->audit_log_path || !server->trusted_proxies ||
        !server->sessions) {
        cbm_log_error("mcp_http.config.invalid", "reason", "out_of_memory");
        cbm_mcp_http_server_free(server);
        return NULL;
    }
    server->session_ttl_sec = config->session_ttl_sec;
    server->max_sessions = config->max_sessions;
    server->max_workers = config->max_workers;
    server->tool_profile = config->tool_profile;
    cbm_mutex_init(&server->sessions_mutex);
    cbm_mutex_init(&server->audit_mutex);
    for (int i = 0; i < server->max_sessions; i++)
        cbm_mutex_init(&server->sessions[i].mutex);
    char token_hash[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(server->auth_token, strlen(server->auth_token), token_hash);
    memcpy(server->token_key_id, token_hash, 12);
    server->token_key_id[12] = '\0';

    FILE *audit = audit_open(server->audit_log_path);
    if (!audit) {
        cbm_log_error("mcp_http.audit.open_failed", "path", server->audit_log_path);
        cbm_mcp_http_server_free(server);
        return NULL;
    }
    fclose(audit);
    server->httpd = cbm_httpd_listen_addr(config->bind_addr, config->port);
    if (!server->httpd) {
        cbm_log_error("mcp_http.listen_failed", "bind", config->bind_addr);
        cbm_mcp_http_server_free(server);
        return NULL;
    }
    return server;
}

void cbm_mcp_http_server_stop(cbm_mcp_http_server_t *server) {
    if (!server)
        return;
    atomic_store(&server->stop, 1);
}

int cbm_mcp_http_server_run(cbm_mcp_http_server_t *server) {
    if (!server || !server->httpd)
        return -1;
    while (!atomic_load(&server->stop)) {
        cbm_http_conn_t *conn = cbm_httpd_accept(server->httpd, ACCEPT_TIMEOUT_MS);
        if (!conn)
            continue;
        if (atomic_load(&server->active_workers) >= server->max_workers) {
            char peer_ip[CLIENT_IP_LEN] = "unknown";
            (void)cbm_http_conn_peer_ip(conn, peer_ip, sizeof(peer_ip));
            char nonce[CBM_SHA256_HEX_LEN + 1];
            make_nonce(server, "busy", peer_ip, nonce);
            cbm_http_replyf(conn, 503, "Retry-After: 1\r\nCache-Control: no-store\r\n", "%s",
                            "server busy");
            write_audit(server, nonce, peer_ip, "", "HTTP", "", "", "server_busy", 503, 0, 0,
                        cbm_http_conn_response_bytes(conn));
            cbm_httpd_conn_close(conn);
            continue;
        }
        cbm_mcp_http_worker_t *worker = malloc(sizeof(*worker));
        if (!worker) {
            cbm_http_replyf(conn, 503, "Retry-After: 1\r\n", "%s", "server busy");
            cbm_httpd_conn_close(conn);
            continue;
        }
        worker->server = server;
        worker->conn = conn;
        atomic_fetch_add(&server->active_workers, 1);
        cbm_thread_t thread;
        if (cbm_thread_create(&thread, 0, connection_worker, worker) != 0) {
            atomic_fetch_sub(&server->active_workers, 1);
            cbm_http_replyf(conn, 503, "Retry-After: 1\r\n", "%s", "server busy");
            cbm_httpd_conn_close(conn);
            free(worker);
            continue;
        }
        (void)cbm_thread_detach(&thread);
    }
    while (atomic_load(&server->active_workers) > 0)
        cbm_usleep(SHUTDOWN_POLL_US);
    return 0;
}

int cbm_mcp_http_server_port(const cbm_mcp_http_server_t *server) {
    return server && server->httpd ? cbm_httpd_port(server->httpd) : -1;
}

void cbm_mcp_http_server_free(cbm_mcp_http_server_t *server) {
    if (!server)
        return;
    if (server->httpd)
        cbm_httpd_close(server->httpd);
    if (server->sessions) {
        for (int i = 0; i < server->max_sessions; i++) {
            session_destroy(&server->sessions[i]);
            cbm_mutex_destroy(&server->sessions[i].mutex);
        }
        free(server->sessions);
    }
    if (server->max_sessions > 0) {
        cbm_mutex_destroy(&server->sessions_mutex);
        cbm_mutex_destroy(&server->audit_mutex);
    }
    if (server->auth_token) {
        memset(server->auth_token, 0, strlen(server->auth_token));
        free(server->auth_token);
    }
    free(server->audit_log_path);
    free(server->trusted_proxies);
    free(server);
}
