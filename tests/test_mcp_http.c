#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "mcp/http_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET test_sock_t;
#define TEST_SOCK_BAD INVALID_SOCKET
#define test_sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int test_sock_t;
#define TEST_SOCK_BAD (-1)
#define test_sock_close close
#endif

static const char TEST_TOKEN[] = "0123456789abcdef0123456789abcdef";

typedef struct {
    cbm_mcp_http_server_t *server;
    cbm_thread_t thread;
} live_server_t;

static void *run_live_server(void *arg) {
    cbm_mcp_http_server_run(arg);
    return NULL;
}

static int live_server_start(live_server_t *live, const char *audit_path) {
    cbm_mcp_http_config_t config = {
        .bind_addr = "127.0.0.1",
        .port = 0,
        .auth_token = TEST_TOKEN,
        .audit_log_path = audit_path,
        .trusted_proxies = "",
        .session_ttl_sec = 60,
        .max_sessions = 4,
        .max_workers = 4,
        .tool_profile = CBM_MCP_TOOL_PROFILE_ANALYSIS,
    };
    live->server = cbm_mcp_http_server_new(&config);
    if (!live->server)
        return -1;
    if (cbm_thread_create(&live->thread, 0, run_live_server, live->server) != 0) {
        cbm_mcp_http_server_free(live->server);
        live->server = NULL;
        return -1;
    }
    return 0;
}

static void live_server_stop(live_server_t *live) {
    cbm_mcp_http_server_stop(live->server);
    cbm_thread_join(&live->thread);
    cbm_mcp_http_server_free(live->server);
    live->server = NULL;
}

static test_sock_t connect_loopback(int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == TEST_SOCK_BAD)
        return TEST_SOCK_BAD;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7f000001U);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        test_sock_close(sock);
        return TEST_SOCK_BAD;
    }
    return sock;
}

static int socket_send_all(test_sock_t sock, const char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
#ifdef _WIN32
        int written = send(sock, data + offset, (int)(len - offset), 0);
#else
        ssize_t written = send(sock, data + offset, len - offset, 0);
#endif
        if (written <= 0)
            return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int http_exchange(int port, const char *request, char *response, size_t response_size) {
    test_sock_t sock = connect_loopback(port);
    if (sock == TEST_SOCK_BAD)
        return -1;
    if (socket_send_all(sock, request, strlen(request)) != 0) {
        test_sock_close(sock);
        return -1;
    }
    size_t used = 0;
    while (used + 1 < response_size) {
#ifdef _WIN32
        int got = recv(sock, response + used, (int)(response_size - used - 1), 0);
#else
        ssize_t got = recv(sock, response + used, response_size - used - 1, 0);
#endif
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    response[used] = '\0';
    test_sock_close(sock);
    return (int)used;
}

static int response_status(const char *response) {
    return response && strncmp(response, "HTTP/1.1 ", 9) == 0 ? atoi(response + 9) : -1;
}

static bool response_header(const char *response, const char *name, char *out, size_t outsz) {
    const char *found = strstr(response, name);
    if (!found)
        return false;
    found += strlen(name);
    while (*found == ' ' || *found == '\t')
        found++;
    const char *end = strstr(found, "\r\n");
    if (!end || (size_t)(end - found) >= outsz)
        return false;
    memcpy(out, found, (size_t)(end - found));
    out[end - found] = '\0';
    return true;
}

static int post_json(int port, const char *token, const char *session, const char *origin,
                     const char *body, char *response, size_t response_size) {
    char request[4096];
    int written = snprintf(
        request, sizeof(request),
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
        "Accept: application/json, text/event-stream\r\n%s%s%s%s%s%sContent-Length: %zu\r\n\r\n%s",
        token ? "Authorization: Bearer " : "", token ? token : "", token ? "\r\n" : "",
        session ? "Mcp-Session-Id: " : "", session ? session : "", session ? "\r\n" : "",
        strlen(body), body);
    if (origin) {
        written =
            snprintf(request, sizeof(request),
                     "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                     "Origin: %s\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n%s",
                     origin, token ? token : "", strlen(body), body);
    }
    if (written < 0 || written >= (int)sizeof(request))
        return -1;
    return http_exchange(port, request, response, response_size);
}

static char *read_file(const char *path) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)size, file);
    data[got] = '\0';
    fclose(file);
    return data;
}

TEST(mcp_http_bearer_auth_is_strict) {
    ASSERT_TRUE(cbm_mcp_http_authorize("Bearer 0123456789abcdef0123456789abcdef", TEST_TOKEN));
    ASSERT_FALSE(cbm_mcp_http_authorize("bearer 0123456789abcdef0123456789abcdef", TEST_TOKEN));
    ASSERT_FALSE(cbm_mcp_http_authorize("Bearer wrong", TEST_TOKEN));
    ASSERT_FALSE(cbm_mcp_http_authorize("Bearer 0123456789abcdef0123456789abcdef ", TEST_TOKEN));
    ASSERT_FALSE(cbm_mcp_http_authorize(NULL, TEST_TOKEN));
    PASS();
}

TEST(mcp_http_proxy_ip_requires_trusted_peer) {
    char resolved[64];
    ASSERT_TRUE(cbm_mcp_http_resolve_client_ip("10.0.0.10", "192.168.1.25", "127.0.0.1/32",
                                               resolved, sizeof(resolved)));
    ASSERT_STR_EQ(resolved, "10.0.0.10");
    ASSERT_TRUE(cbm_mcp_http_resolve_client_ip("10.0.0.10", "192.168.1.25, 10.0.0.9", "10.0.0.0/24",
                                               resolved, sizeof(resolved)));
    ASSERT_STR_EQ(resolved, "192.168.1.25");
    ASSERT_TRUE(cbm_mcp_http_resolve_client_ip("10.0.0.10", "not-an-ip", "10.0.0.10", resolved,
                                               sizeof(resolved)));
    ASSERT_STR_EQ(resolved, "10.0.0.10");
    PASS();
}

TEST(mcp_http_config_fails_closed) {
    cbm_mcp_http_config_t config = {
        .bind_addr = "127.0.0.1",
        .port = 0,
        .auth_token = "short",
        .audit_log_path = "unused-audit.jsonl",
        .session_ttl_sec = 60,
        .max_sessions = 2,
        .max_workers = 2,
        .tool_profile = CBM_MCP_TOOL_PROFILE_ANALYSIS,
    };
    ASSERT_NULL(cbm_mcp_http_server_new(&config));
    config.auth_token = TEST_TOKEN;
    config.tool_profile = CBM_MCP_TOOL_PROFILE_ALL;
    ASSERT_NULL(cbm_mcp_http_server_new(&config));
    PASS();
}

TEST(mcp_http_live_auth_session_tools_and_audit) {
    char tempdir[256] = "build/c/cbm-mcp-http-XXXXXX";
    ASSERT_NOT_NULL(cbm_mkdtemp(tempdir));
    char audit_path[1024];
    snprintf(audit_path, sizeof(audit_path), "%s/audit.jsonl", tempdir);

    live_server_t live;
    memset(&live, 0, sizeof(live));
    ASSERT_EQ(live_server_start(&live, audit_path), 0);
    int port = cbm_mcp_http_server_port(live.server);
    ASSERT_GT(port, 0);

    const char *initialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},\"clientInfo\":{"
        "\"name\":\"test\",\"version\":\"1\"}}}";
    char *response = malloc(65536);
    ASSERT_NOT_NULL(response);

    ASSERT_GT(post_json(port, NULL, NULL, NULL, initialize, response, 65536), 0);
    ASSERT_EQ(response_status(response), 401);
    ASSERT_NOT_NULL(strstr(response, "WWW-Authenticate: Bearer"));

    ASSERT_GT(
        post_json(port, TEST_TOKEN, NULL, "https://evil.invalid", initialize, response, 65536), 0);
    ASSERT_EQ(response_status(response), 403);

    ASSERT_GT(post_json(port, TEST_TOKEN, NULL, NULL, initialize, response, 65536), 0);
    ASSERT_EQ(response_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "\"protocolVersion\":\"2025-03-26\""));
    char session[128];
    ASSERT_TRUE(response_header(response, "Mcp-Session-Id:", session, sizeof(session)));
    ASSERT_EQ((int)strlen(session), 64);

    const char *tools = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}";
    ASSERT_GT(post_json(port, TEST_TOKEN, session, NULL, tools, response, 65536), 0);
    ASSERT_EQ(response_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "\"name\":\"search_graph\""));
    ASSERT_NULL(strstr(response, "\"name\":\"index_repository\""));

    ASSERT_GT(post_json(port, TEST_TOKEN, NULL, NULL, tools, response, 65536), 0);
    ASSERT_EQ(response_status(response), 404);
    free(response);

    live_server_stop(&live);
    char *audit = read_file(audit_path);
    ASSERT_NOT_NULL(audit);
    ASSERT_NOT_NULL(strstr(audit, "\"client_ip\":\"127.0.0.1\""));
    ASSERT_NOT_NULL(strstr(audit, "\"method\":\"initialize\""));
    ASSERT_NOT_NULL(strstr(audit, "\"method\":\"tools/list\""));
    ASSERT_NOT_NULL(strstr(audit, "\"status\":\"auth_failed\""));
    ASSERT_NULL(strstr(audit, TEST_TOKEN));
    free(audit);
    th_cleanup(tempdir);
    PASS();
}

SUITE(mcp_http) {
    RUN_TEST(mcp_http_bearer_auth_is_strict);
    RUN_TEST(mcp_http_proxy_ip_requires_trusted_peer);
    RUN_TEST(mcp_http_config_fails_closed);
    RUN_TEST(mcp_http_live_auth_session_tools_and_audit);
}
