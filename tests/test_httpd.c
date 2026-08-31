/*
 * test_httpd.c — Tests for the first-party graph-UI HTTP server.
 *
 * Two layers:
 *   1. Parser/helper unit tests against httpd.h's pure functions
 *      (no sockets): request-line parsing, strict CRLF, Content-Length
 *      edge cases, chunked rejection, NUL/percent-decode rules,
 *      query-param decoding, route pattern matching.
 *   2. Live-socket integration tests against the full UI server
 *      (http_server.c) on an ephemeral port: routing, CORS policy,
 *      RPC dispatch, transport limits, receive deadline, clean shutdown.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/compat_thread.h"
#include "../src/foundation/log.h"
#include "../src/foundation/platform.h"
#include "../src/cli/cli.h"
#include "../src/git/git_context.h" /* #798 follow-up: live-socket git-resolve repro */
#include "../src/git/remote_repo.h"
#include "../src/ui/http_server.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "ui/httpd.h"
#include "ui/http_server.h"
#include <store/store.h>
#include <watcher/watcher.h>

#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h> /* #798 follow-up: CreateThread/WaitForSingleObject watchdog */
typedef SOCKET th_sock_t;
#define th_sock_close closesocket
#define TH_SOCK_BAD INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h> /* struct timeval for the SO_RCVTIMEO watchdog (#798 follow-up) */
#include <sys/wait.h> /* fork/waitpid crash-isolation for the browse overflow guard */
#include <unistd.h>
typedef int th_sock_t;
#define th_sock_close close
#define TH_SOCK_BAD (-1)
#endif

static char httpd_log_buf[8192];

static void httpd_capture_log(const char *line) {
    size_t used = strlen(httpd_log_buf);
    size_t avail = sizeof(httpd_log_buf) - used;
    if (avail <= 1)
        return;
    int n = snprintf(httpd_log_buf + used, avail, "%s\n", line ? line : "");
    if (n < 0 || (size_t)n >= avail)
        httpd_log_buf[sizeof(httpd_log_buf) - 1] = '\0';
}

/* ── Raw-socket test client ───────────────────────────────────── */

static th_sock_t th_connect(int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa); /* refcounted; cleanup not needed in tests */
#endif
    th_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == TH_SOCK_BAD)
        return TH_SOCK_BAD;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        th_sock_close(s);
        return TH_SOCK_BAD;
    }
    return s;
}

static int th_send_all(th_sock_t s, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int n = send(s, data + off, (int)(len - off), 0);
#else
        ssize_t n = send(s, data + off, len - off, 0);
#endif
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Read until the server closes the connection (Connection: close model). */
static int th_recv_until_close(th_sock_t s, char *buf, size_t bufsz) {
    size_t off = 0;
    for (;;) {
#ifdef _WIN32
        int n = recv(s, buf + off, (int)(bufsz - 1 - off), 0);
#else
        ssize_t n = recv(s, buf + off, bufsz - 1 - off, 0);
#endif
        if (n <= 0)
            break;
        off += (size_t)n;
        if (off >= bufsz - 1)
            break;
    }
    buf[off] = '\0';
    return (int)off;
}

/* One-shot HTTP exchange. Returns response length, 0 on connect failure. */
static int th_http(int port, const char *request, char *resp, size_t respsz) {
    th_sock_t s = th_connect(port);
    if (s == TH_SOCK_BAD)
        return 0;
    if (th_send_all(s, request, strlen(request)) != 0) {
        th_sock_close(s);
        return 0;
    }
    int n = th_recv_until_close(s, resp, respsz);
    th_sock_close(s);
    return n;
}

/* HTTP status code from a raw response ("HTTP/1.1 404 ..."), or -1. */
static int th_status(const char *resp) {
    if (strncmp(resp, "HTTP/1.1 ", 9) != 0)
        return -1;
    return atoi(resp + 9);
}

/* ── Parser unit tests ────────────────────────────────────────── */

TEST(httpd_parse_simple_get) {
    const char *raw = "GET /api/logs?lines=5 HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Origin: http://localhost:5173\r\n"
                      "\r\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 99;
    int rc = cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "GET");
    ASSERT_STR_EQ(req.path, "/api/logs");
    ASSERT_STR_EQ(req.query, "lines=5");
    ASSERT_STR_EQ(req.origin, "http://localhost:5173");
    ASSERT_EQ((int)clen, 0);
    ASSERT_EQ((int)body_off, (int)strlen(raw));
    PASS();
}

TEST(httpd_parse_post_with_body_offset) {
    const char *raw = "POST /rpc HTTP/1.1\r\n"
                      "Content-Length: 7\r\n"
                      "Content-Type: application/json\r\n"
                      "\r\n"
                      "{\"a\":1}";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    int rc = cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "POST");
    ASSERT_STR_EQ(req.path, "/rpc");
    ASSERT_STR_EQ(req.query, "");
    ASSERT_EQ((int)clen, 7);
    ASSERT_STR_EQ(raw + body_off, "{\"a\":1}");
    PASS();
}

TEST(httpd_parse_origin_case_insensitive) {
    const char *raw = "GET / HTTP/1.1\r\n"
                      "origin: http://127.0.0.1:9749\r\n"
                      "\r\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 0);
    ASSERT_STR_EQ(req.origin, "http://127.0.0.1:9749");
    PASS();
}

TEST(httpd_parse_rejects_bare_lf) {
    const char *raw = "GET / HTTP/1.1\nHost: x\n\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 400);
    PASS();
}

TEST(httpd_parse_rejects_chunked) {
    const char *raw = "POST /rpc HTTP/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 411);
    PASS();
}

TEST(httpd_parse_rejects_oversized_content_length) {
    char raw[256];
    snprintf(raw, sizeof(raw), "POST /rpc HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
             CBM_HTTP_MAX_BODY + 1);
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 413);
    PASS();
}

TEST(httpd_parse_rejects_garbage_content_length) {
    const char *raw = "POST /rpc HTTP/1.1\r\nContent-Length: abc\r\n\r\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 400);

    const char *neg = "POST /rpc HTTP/1.1\r\nContent-Length: -5\r\n\r\n";
    ASSERT_EQ(cbm_http_parse_head(neg, strlen(neg), &req, &body_off, &clen), 400);
    PASS();
}

TEST(httpd_parse_rejects_percent00_in_target) {
    const char *raw = "GET /a%00b HTTP/1.1\r\n\r\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 400);

    /* %00 hidden in the query string is rejected too */
    const char *q = "GET /ok?x=%00 HTTP/1.1\r\n\r\n";
    ASSERT_EQ(cbm_http_parse_head(q, strlen(q), &req, &body_off, &clen), 400);
    PASS();
}

TEST(httpd_parse_rejects_raw_nul_in_head) {
    char raw[64] = "GET /a";
    size_t len = 6;
    raw[len++] = '\0';
    memcpy(raw + len, " HTTP/1.1\r\n\r\n", 13);
    len += 13;
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, len, &req, &body_off, &clen), 400);
    PASS();
}

TEST(httpd_parse_incomplete_head_needs_more) {
    const char *raw = "GET /api/logs HTTP/1.1\r\nHost: x\r\n"; /* no CRLFCRLF yet */
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), CBM_HTTP_NEED_MORE);
    PASS();
}

TEST(httpd_parse_rejects_missing_version) {
    const char *raw = "GET /\r\n\r\n";
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    ASSERT_EQ(cbm_http_parse_head(raw, strlen(raw), &req, &body_off, &clen), 400);

    const char *v2 = "GET / HTTP/2\r\n\r\n";
    ASSERT_EQ(cbm_http_parse_head(v2, strlen(v2), &req, &body_off, &clen), 400);
    PASS();
}

TEST(httpd_parse_rejects_oversized_head) {
    /* A head that exceeds CBM_HTTP_MAX_HEAD without terminating → 431 */
    size_t big = CBM_HTTP_MAX_HEAD + 1024;
    char *raw = malloc(big);
    ASSERT_NOT_NULL(raw);
    memcpy(raw, "GET / HTTP/1.1\r\nX-Junk: ", 24);
    memset(raw + 24, 'A', big - 24);
    cbm_http_req_t req;
    size_t body_off = 0, clen = 0;
    int rc = cbm_http_parse_head(raw, big, &req, &body_off, &clen);
    free(raw);
    ASSERT_EQ(rc, 431);
    PASS();
}

TEST(httpd_query_param_decode) {
    char buf[64];
    ASSERT_TRUE(cbm_http_query_param("a=hello+world&b=%2Ffoo%2F", "a", buf, (int)sizeof(buf)));
    ASSERT_STR_EQ(buf, "hello world");
    ASSERT_TRUE(cbm_http_query_param("a=hello+world&b=%2Ffoo%2F", "b", buf, (int)sizeof(buf)));
    ASSERT_STR_EQ(buf, "/foo/");
    /* uppercase + lowercase hex */
    ASSERT_TRUE(cbm_http_query_param("p=%2fTmp%2F", "p", buf, (int)sizeof(buf)));
    ASSERT_STR_EQ(buf, "/Tmp/");
    PASS();
}

TEST(httpd_query_param_edge_cases) {
    char buf[8];
    /* missing param */
    ASSERT_FALSE(cbm_http_query_param("a=1", "b", buf, (int)sizeof(buf)));
    /* empty value (current server treats it as absent) */
    ASSERT_FALSE(cbm_http_query_param("a=&b=2", "a", buf, (int)sizeof(buf)));
    /* value too large for buf */
    ASSERT_FALSE(cbm_http_query_param("a=123456789", "a", buf, (int)sizeof(buf)));
    /* decoded NUL rejected */
    char big[32];
    ASSERT_FALSE(cbm_http_query_param("a=x%00y", "a", big, (int)sizeof(big)));
    /* name is a prefix of another name — must not match */
    ASSERT_FALSE(cbm_http_query_param("abc=1", "ab", buf, (int)sizeof(buf)));
    /* truncated percent escape */
    ASSERT_FALSE(cbm_http_query_param("a=%2", "a", buf, (int)sizeof(buf)));
    PASS();
}

TEST(httpd_path_match_matrix) {
    /* exact */
    ASSERT_TRUE(cbm_http_path_match("/", "/"));
    ASSERT_FALSE(cbm_http_path_match("/x", "/"));
    ASSERT_TRUE(cbm_http_path_match("/rpc", "/rpc"));
    ASSERT_FALSE(cbm_http_path_match("/rpc2", "/rpc"));
    /* trailing-* prefix */
    ASSERT_TRUE(cbm_http_path_match("/api/layout", "/api/layout*"));
    ASSERT_TRUE(cbm_http_path_match("/assets/index-abc.js", "/assets/*"));
    ASSERT_FALSE(cbm_http_path_match("/api/browse", "/api/layout*"));
    /* raw path is matched — percent-encoded slash must NOT route */
    ASSERT_FALSE(cbm_http_path_match("/api%2Fbrowse", "/api/browse*"));
    ASSERT_FALSE(cbm_http_path_match("/api%2fbrowse", "/api/browse*"));
    /* CORS origin patterns */
    ASSERT_TRUE(cbm_http_path_match("http://localhost:5173", "http://localhost:*"));
    ASSERT_TRUE(cbm_http_path_match("http://127.0.0.1:9749", "http://127.0.0.1:*"));
    ASSERT_FALSE(cbm_http_path_match("http://evil.com", "http://localhost:*"));
    ASSERT_FALSE(cbm_http_path_match("https://localhost:5173", "http://localhost:*"));
    ASSERT_FALSE(cbm_http_path_match("http://localhost.evil.com:80", "http://localhost:*"));
    PASS();
}

TEST(httpd_resolves_bare_binary_path_from_path) {
#ifdef _WIN32
    PASS();
#else
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_httpd_bin_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char exe[512];
    snprintf(exe, sizeof(exe), "%s/codebase-memory-mcp", td);
    FILE *f = fopen(exe, "w");
    ASSERT_NOT_NULL(f);
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    ASSERT_EQ(chmod(exe, 0755), 0);

    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    cbm_setenv("PATH", td, 1);

    char resolved[1024];
    ASSERT_TRUE(
        cbm_http_server_resolve_binary_path("codebase-memory-mcp", resolved, sizeof(resolved)));
    ASSERT_STR_EQ(resolved, exe);

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    } else {
        cbm_unsetenv("PATH");
    }
    PASS();
#endif
}

/* ── Transport integration (listener only) ────────────────────── */

TEST(httpd_listen_ephemeral_port) {
    cbm_httpd_t *d = cbm_httpd_listen(0);
    ASSERT_NOT_NULL(d);
    int port = cbm_httpd_port(d);
    ASSERT_GT(port, 0);
    /* accept with a short timeout and no client → NULL, promptly */
    cbm_http_conn_t *c = cbm_httpd_accept(d, 50);
    ASSERT_NULL(c);
    cbm_httpd_close(d);
    PASS();
}

TEST(httpd_listen_port_collision_returns_null) {
    cbm_httpd_t *d1 = cbm_httpd_listen(0);
    ASSERT_NOT_NULL(d1);
    cbm_httpd_t *d2 = cbm_httpd_listen(cbm_httpd_port(d1));
    ASSERT_NULL(d2);
    cbm_httpd_close(d1);
    PASS();
}

/* ── Full UI server integration ───────────────────────────────── */

typedef struct {
    cbm_http_server_t *srv;
    cbm_thread_t tid;
} th_server_t;

static void *th_server_thread(void *arg) {
    cbm_http_server_run((cbm_http_server_t *)arg);
    return NULL;
}

static int th_server_start(th_server_t *ts) {
    ts->srv = cbm_http_server_new(0);
    if (!ts->srv)
        return -1;
    if (cbm_thread_create(&ts->tid, 0, th_server_thread, ts->srv) != 0) {
        cbm_http_server_free(ts->srv);
        return -1;
    }
    return 0;
}

static int th_server_start_with_watcher(th_server_t *ts, cbm_watcher_t *watcher) {
    ts->srv = cbm_http_server_new(0);
    if (!ts->srv)
        return -1;
    cbm_http_server_set_watcher(ts->srv, watcher);
    if (cbm_thread_create(&ts->tid, 0, th_server_thread, ts->srv) != 0) {
        cbm_http_server_free(ts->srv);
        return -1;
    }
    return 0;
}

static void th_server_stop(th_server_t *ts) {
    cbm_http_server_stop(ts->srv);
    cbm_thread_join(&ts->tid);
    cbm_http_server_free(ts->srv);
}

typedef struct {
    char tmpdir[256];
    char cache_dir[512];
    char root_dir[512];
    char *saved_cache_dir;
    cbm_store_t *store;
    cbm_watcher_t *watcher;
} ui_delete_fixture_t;

static int ui_delete_fixture_init(ui_delete_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    char *tmp = th_mktempdir("cbm_httpd_delete");
    if (!tmp)
        return -1;
    snprintf(fx->tmpdir, sizeof(fx->tmpdir), "%s", tmp);
    snprintf(fx->cache_dir, sizeof(fx->cache_dir), "%s/cache", fx->tmpdir);
    snprintf(fx->root_dir, sizeof(fx->root_dir), "%s/root", fx->tmpdir);

    const char *saved = getenv("CBM_CACHE_DIR");
    fx->saved_cache_dir = saved ? strdup(saved) : NULL;
    if (th_mkdir_p(fx->cache_dir) != 0 || th_mkdir_p(fx->root_dir) != 0) {
        return -1;
    }
    cbm_setenv("CBM_CACHE_DIR", fx->cache_dir, 1);

    fx->store = cbm_store_open_memory();
    fx->watcher = cbm_watcher_new(fx->store, NULL, NULL);
    return fx->store && fx->watcher ? 0 : -1;
}

static void ui_delete_fixture_cleanup(ui_delete_fixture_t *fx) {
    if (fx->watcher)
        cbm_watcher_free(fx->watcher);
    if (fx->store)
        cbm_store_close(fx->store);
    if (fx->saved_cache_dir) {
        cbm_setenv("CBM_CACHE_DIR", fx->saved_cache_dir, 1);
        free(fx->saved_cache_dir);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_cleanup(fx->tmpdir);
}

static void ui_delete_db_path(const ui_delete_fixture_t *fx, const char *project, char *out,
                              size_t outsz) {
    snprintf(out, outsz, "%s/%s.db", fx->cache_dir, project);
}

static int ui_delete_make_db_file(const ui_delete_fixture_t *fx, const char *project) {
    char path[1024];
    ui_delete_db_path(fx, project, path, sizeof(path));
    return th_write_file(path, "test db");
}

static int ui_delete_make_project_db(const ui_delete_fixture_t *fx, const char *project,
                                     const char *root_path) {
    char path[1024];
    ui_delete_db_path(fx, project, path, sizeof(path));
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return -1;
    }
    int result = cbm_store_upsert_project(store, project, root_path);
    cbm_store_close(store);
    return result == CBM_STORE_OK ? 0 : -1;
}

static int ui_delete_make_sidecars(const ui_delete_fixture_t *fx, const char *project) {
    char path[1024];
    ui_delete_db_path(fx, project, path, sizeof(path));
    char wal[1040], shm[1040];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);
    return th_write_file(wal, "wal") == 0 && th_write_file(shm, "shm") == 0 ? 0 : -1;
}

static int ui_delete_request(th_server_t *ts, const char *target, char *resp, size_t respsz) {
    char req[512];
    snprintf(req, sizeof(req), "DELETE %s HTTP/1.1\r\n\r\n", target);
    return th_http(cbm_http_server_port(ts->srv), req, resp, respsz);
}

static bool ui_layout_json_has_node(yyjson_val *nodes, int64_t id) {
    size_t idx, max;
    yyjson_val *node;
    yyjson_arr_foreach(nodes, idx, max, node) {
        yyjson_val *node_id = yyjson_obj_get(node, "id");
        if (node_id && yyjson_get_sint(node_id) == id)
            return true;
    }
    return false;
}

/* Java as the primary project must retain both caller endpoints and return
 * independently sampled Web + Python satellites. The target handler QNs are
 * intentionally different from the local Route QNs, reproducing the old
 * cross_edges=0 behavior. */
TEST(ui_server_layout_returns_two_linked_projects_with_renderable_cross_edges) {
    /* Keep the SQLite fixture path ASCII even when the workspace's absolute
     * Windows path contains non-ASCII user-name characters. */
    char temp_pattern[] = "build/c/cbm_httpd_layout_XXXXXX";
    char *tmpdir = cbm_mkdtemp(temp_pattern);
    ASSERT_NOT_NULL(tmpdir);
    char cache_dir[512];
    /* The freshly-created temp directory can serve directly as CBM_CACHE_DIR.
     * This also avoids platform-specific mixed-separator mkdir behavior. */
    snprintf(cache_dir, sizeof(cache_dir), "%s", tmpdir);
    const char *old_cache_raw = getenv("CBM_CACHE_DIR");
    char *old_cache = old_cache_raw ? strdup(old_cache_raw) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache_dir, 1);

    char path[1024];
    snprintf(path, sizeof(path), "%s/java.db", cache_dir);
    cbm_store_t *java = cbm_store_open_path(path);
    ASSERT_NOT_NULL(java);
    ASSERT_EQ(cbm_store_upsert_project(java, "java", "/tmp/java"), CBM_STORE_OK);
    cbm_node_t java_web = {.project = "java",
                           .label = "Method",
                           .name = "javaToWeb",
                           .qualified_name = "java.Controller.javaToWeb",
                           .file_path = "src/Controller.java"};
    cbm_node_t java_python = {.project = "java",
                              .label = "Method",
                              .name = "javaToPython",
                              .qualified_name = "java.Controller.javaToPython",
                              .file_path = "src/Controller.java"};
    cbm_node_t web_route = {.project = "java",
                            .label = "Route",
                            .name = "POST /web",
                            .qualified_name = "__route__POST/web"};
    cbm_node_t python_route = {.project = "java",
                               .label = "Route",
                               .name = "POST /python",
                               .qualified_name = "__route__POST/python"};
    int64_t java_web_id = cbm_store_upsert_node(java, &java_web);
    int64_t java_python_id = cbm_store_upsert_node(java, &java_python);
    int64_t web_route_id = cbm_store_upsert_node(java, &web_route);
    int64_t python_route_id = cbm_store_upsert_node(java, &python_route);
    ASSERT_GT(java_web_id, 0);
    ASSERT_GT(java_python_id, 0);
    cbm_edge_t to_web = {.project = "java",
                         .source_id = java_web_id,
                         .target_id = web_route_id,
                         .type = "CROSS_HTTP_CALLS",
                         .properties_json =
                             "{\"target_project\":\"web\",\"target_function\":\"callJava\","
                             "\"target_file\":\"src/api.ts\"}"};
    cbm_edge_t to_python = {.project = "java",
                            .source_id = java_python_id,
                            .target_id = python_route_id,
                            .type = "CROSS_HTTP_CALLS",
                            .properties_json =
                                "{\"target_project\":\"python\",\"target_function\":\"call_java\","
                                "\"target_file\":\"server/client.py\"}"};
    ASSERT_GT(cbm_store_insert_edge(java, &to_web), 0);
    ASSERT_GT(cbm_store_insert_edge(java, &to_python), 0);
    cbm_store_close(java);

    snprintf(path, sizeof(path), "%s/web.db", cache_dir);
    cbm_store_t *web = cbm_store_open_path(path);
    ASSERT_NOT_NULL(web);
    ASSERT_EQ(cbm_store_upsert_project(web, "web", "/tmp/web"), CBM_STORE_OK);
    cbm_node_t web_noise1 = {
        .project = "web", .label = "Function", .name = "aaa", .qualified_name = "web.aaa"};
    cbm_node_t web_noise2 = {
        .project = "web", .label = "Function", .name = "bbb", .qualified_name = "web.bbb"};
    cbm_node_t web_handler = {.project = "web",
                              .label = "Function",
                              .name = "callJava",
                              .qualified_name = "web.api.callJava",
                              .file_path = "src/api.ts"};
    cbm_store_upsert_node(web, &web_noise1);
    cbm_store_upsert_node(web, &web_noise2);
    int64_t web_handler_id = cbm_store_upsert_node(web, &web_handler);
    ASSERT_GT(web_handler_id, 0);
    cbm_store_close(web);

    snprintf(path, sizeof(path), "%s/python.db", cache_dir);
    cbm_store_t *python = cbm_store_open_path(path);
    ASSERT_NOT_NULL(python);
    ASSERT_EQ(cbm_store_upsert_project(python, "python", "/tmp/python"), CBM_STORE_OK);
    cbm_node_t python_noise1 = {
        .project = "python", .label = "Function", .name = "aaa", .qualified_name = "python.aaa"};
    cbm_node_t python_noise2 = {
        .project = "python", .label = "Function", .name = "bbb", .qualified_name = "python.bbb"};
    cbm_node_t python_handler = {.project = "python",
                                 .label = "Function",
                                 .name = "call_java",
                                 .qualified_name = "python.client.call_java",
                                 .file_path = "server/client.py"};
    cbm_store_upsert_node(python, &python_noise1);
    cbm_store_upsert_node(python, &python_noise2);
    int64_t python_handler_id = cbm_store_upsert_node(python, &python_handler);
    ASSERT_GT(python_handler_id, 0);
    cbm_store_close(python);

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char response[65536];
    int response_len = th_http(cbm_http_server_port(ts.srv),
                               "GET /api/layout?project=java&max_nodes=2 HTTP/1.1\r\n\r\n",
                               response, sizeof(response));
    ASSERT_GT(response_len, 0);
    ASSERT_EQ(th_status(response), 200);
    th_server_stop(&ts);

    const char *body = strstr(response, "\r\n\r\n");
    ASSERT_NOT_NULL(body);
    body += 4;
    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *primary_nodes = yyjson_obj_get(root, "nodes");
    ASSERT_TRUE(ui_layout_json_has_node(primary_nodes, java_web_id));
    ASSERT_TRUE(ui_layout_json_has_node(primary_nodes, java_python_id));

    yyjson_val *linked = yyjson_obj_get(root, "linked_projects");
    ASSERT_EQ(yyjson_arr_size(linked), 2);
    bool found_web = false, found_python = false;
    size_t idx, max;
    yyjson_val *entry;
    yyjson_arr_foreach(linked, idx, max, entry) {
        const char *project = yyjson_get_str(yyjson_obj_get(entry, "project"));
        yyjson_val *cross_edges = yyjson_obj_get(entry, "cross_edges");
        ASSERT_EQ(yyjson_arr_size(cross_edges), 1);
        yyjson_val *edge = yyjson_arr_get_first(cross_edges);
        int64_t target = yyjson_get_sint(yyjson_obj_get(edge, "target"));
        yyjson_val *nodes = yyjson_obj_get(entry, "nodes");
        ASSERT_TRUE(ui_layout_json_has_node(nodes, target));
        if (project && strcmp(project, "web") == 0) {
            found_web = target == web_handler_id;
        } else if (project && strcmp(project, "python") == 0) {
            found_python = target == python_handler_id;
        }
    }
    ASSERT_TRUE(found_web);
    ASSERT_TRUE(found_python);
    yyjson_doc_free(doc);

    if (old_cache) {
        cbm_setenv("CBM_CACHE_DIR", old_cache, 1);
        free(old_cache);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_cleanup(tmpdir);
    PASS();
}

TEST(ui_server_unknown_path_404) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    int port = cbm_http_server_port(ts.srv);

    char resp[4096];
    int n = th_http(port, "GET /definitely/not/here HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 404);
    /* every response is explicit-length + close */
    ASSERT_NOT_NULL(strstr(resp, "Connection: close"));
    ASSERT_NOT_NULL(strstr(resp, "Content-Length:"));

    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_admin_probes_and_jobs) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    int port = cbm_http_server_port(ts.srv);
    char resp[16384];

    int n = th_http(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ok\""));

    n = th_http(port, "GET /readyz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ready\""));

    n = th_http(port, "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "cbm_index_jobs_active"));

    n = th_http(port, "GET /admin/v1/jobs HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "["));

    n = th_http(port, "GET /admin/v1/projects HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", resp,
                sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"projects\""));

    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_root_serves_stub_404) {
    /* Test binary links embedded_stub.c → no frontend → 404 with marker */
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv), "GET / HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 404);
    ASSERT_NOT_NULL(strstr(resp, "no frontend embedded"));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_cors_localhost_reflected) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "OPTIONS /rpc HTTP/1.1\r\n"
                    "Origin: http://localhost:5173\r\n\r\n",
                    resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 204);
    ASSERT_NOT_NULL(strstr(resp, "Access-Control-Allow-Origin: http://localhost:5173"));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_cors_evil_origin_not_reflected) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "OPTIONS /rpc HTTP/1.1\r\n"
                    "Origin: http://evil.example.com\r\n\r\n",
                    resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 204);
    ASSERT_NULL(strstr(resp, "Access-Control-Allow-Origin"));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_integrations_returns_read_only_plan) {
    cbm_http_server_set_binary_path("codebase-memory-mcp-test");
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[65536];
    int n =
        th_http(cbm_http_server_port(ts.srv),
                "GET /api/integrations HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"type\": \"agent.install.plan.v1\""));
    ASSERT_NOT_NULL(strstr(resp, "\"writes_started\": false"));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_integrations_apply_requires_explicit_confirmation) {
    cbm_http_server_set_binary_path("codebase-memory-mcp-test");
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    const char *body = "{\"confirm\":false}";
    char req[512];
    snprintf(req, sizeof(req),
             "POST /api/integrations/apply HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
             "Content-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv), req, resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 400);
    ASSERT_NOT_NULL(strstr(resp, "explicit confirmation required"));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_rpc_initialize) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"protocolVersion\":\"2024-11-05\","
                       "\"capabilities\":{},"
                       "\"clientInfo\":{\"name\":\"t\",\"version\":\"0\"}}}";
    char req[1024];
    snprintf(req, sizeof(req),
             "POST /rpc HTTP/1.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    char resp[8192];
    int n = th_http(cbm_http_server_port(ts.srv), req, resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"jsonrpc\""));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_oversized_body_rejected) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char req[256];
    snprintf(req, sizeof(req), "POST /rpc HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
             CBM_HTTP_MAX_BODY + 1);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv), req, resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 413);
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_encoded_slash_not_routed) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv), "GET /api%2Fbrowse?path=/tmp HTTP/1.1\r\n\r\n",
                    resp, sizeof(resp));
    ASSERT_GT(n, 0);
    /* must fall through to 404 — NOT the browse handler */
    ASSERT_EQ(th_status(resp), 404);
    ASSERT_NULL(strstr(resp, "\"dirs\""));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_nul_in_target_rejected) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n =
        th_http(cbm_http_server_port(ts.srv), "GET /a%00b HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 400);
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_browse_traversal_probe) {
    /* Percent-encoded traversal in the QUERY VALUE is decoded (that is the
     * documented contract) and then hits the same directory checks as any
     * other path. The server must answer with a well-formed JSON error or
     * listing — never crash, never echo raw unescaped input. */
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[65536];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "GET /api/browse?path=%2Ftmp%2F..%2F..%2Fprivate HTTP/1.1\r\n\r\n", resp,
                    sizeof(resp));
    ASSERT_GT(n, 0);
    int st = th_status(resp);
    ASSERT_TRUE(st == 200 || st == 400 || st == 403);
    const char *json = strstr(resp, "\r\n\r\n");
    ASSERT_NOT_NULL(json);
    ASSERT_EQ(json[4], '{');
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_browse_utf8_directory) {
    char *created = th_mktempdir("cbm_browse_utf8");
    if (!created) {
        FAIL("temp directory creation failed");
    }
    char base[512];
    char utf8_dir[768];
    snprintf(base, sizeof(base), "%s", created);
    snprintf(utf8_dir, sizeof(utf8_dir), "%s/中文目录", base);
    if (th_mkdir_p(utf8_dir) != 0) {
        th_rmtree(base);
        FAIL("UTF-8 directory creation failed");
    }

    th_server_t ts;
    if (th_server_start(&ts) != 0) {
        th_rmtree(base);
        FAIL("HTTP server start failed");
    }
    char request[2048];
    char response[8192];
    snprintf(request, sizeof(request),
             "GET /api/browse?path=%s/%%E4%%B8%%AD%%E6%%96%%87%%E7%%9B%%AE%%E5%%BD%%95 "
             "HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
             base);
    int n = th_http(cbm_http_server_port(ts.srv), request, response, sizeof(response));
    th_server_stop(&ts);
    th_rmtree(base);

    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "中文目录"));
    PASS();
}

TEST(ui_server_project_update_requires_project_name) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "POST /api/project-update HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Length: 0\r\n\r\n",
                    resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 400);
    ASSERT_NOT_NULL(strstr(resp, "\"error\":\"missing project\""));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_project_update_rejects_unknown_project) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "POST /api/project-update?name=does-not-exist HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Length: 0\r\n\r\n",
                    resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 404);
    ASSERT_NOT_NULL(strstr(resp, "\"error\":\"project not found\""));
    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_delete_project_unwatches_after_delete) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    ASSERT_EQ(ui_delete_make_db_file(&fx, "ui-delete-watch"), 0);
    ASSERT_EQ(ui_delete_make_sidecars(&fx, "ui-delete-watch"), 0);
    cbm_watcher_watch(fx.watcher, "ui-delete-watch", fx.root_dir);
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-watch", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"deleted\""));

    char db[1024], wal[1040], shm[1040];
    ui_delete_db_path(&fx, "ui-delete-watch", db, sizeof(db));
    snprintf(wal, sizeof(wal), "%s-wal", db);
    snprintf(shm, sizeof(shm), "%s-shm", db);
    ASSERT_FALSE(cbm_file_exists(db));
    ASSERT_FALSE(cbm_file_exists(wal));
    ASSERT_FALSE(cbm_file_exists(shm));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 0);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_unicode_cache_path) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);

    snprintf(fx.cache_dir, sizeof(fx.cache_dir), "%s/%s", fx.tmpdir, "\xE7\xBC\x93\xE5\xAD\x98");
    ASSERT_EQ(th_mkdir_p(fx.cache_dir), 0);
#ifdef _WIN32
    wchar_t wide_cache[512];
    ASSERT_GT(MultiByteToWideChar(CP_UTF8, 0, fx.cache_dir, -1, wide_cache,
                                  (int)(sizeof(wide_cache) / sizeof(wide_cache[0]))),
              0);
    ASSERT_TRUE(SetEnvironmentVariableW(L"CBM_CACHE_DIR", wide_cache));
#else
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", fx.cache_dir, 1), 0);
#endif
    ASSERT_EQ(ui_delete_make_db_file(&fx, "ui-delete-unicode"), 0);

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-unicode", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"deleted\""));

    char db[1024];
    ui_delete_db_path(&fx, "ui-delete-unicode", db, sizeof(db));
    ASSERT_FALSE(cbm_file_exists(db));

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_preserves_local_source) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    ASSERT_EQ(ui_delete_make_project_db(&fx, "ui-delete-local", fx.root_dir), 0);
    char sentinel[1024];
    snprintf(sentinel, sizeof(sentinel), "%s/source.txt", fx.root_dir);
    ASSERT_EQ(th_write_file(sentinel, "keep me"), 0);
    cbm_watcher_watch(fx.watcher, "ui-delete-local", fx.root_dir);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-local", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_TRUE(cbm_is_dir(fx.root_dir));
    ASSERT_TRUE(cbm_file_exists(sentinel));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 0);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_removes_managed_clone) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    const char *project = "ui-delete-remote";
    char managed_root[1024];
    ASSERT_TRUE(cbm_remote_repo_managed_path(project, managed_root, sizeof(managed_root)));

    char git_dir[1100];
    char marker[1200];
    char nested_dir[1100];
    char nested_file[1200];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", managed_root);
    snprintf(marker, sizeof(marker), "%s/cbm-remote.json", git_dir);
    snprintf(nested_dir, sizeof(nested_dir), "%s/src/nested", managed_root);
    snprintf(nested_file, sizeof(nested_file), "%s/example.txt", nested_dir);
    ASSERT_EQ(th_mkdir_p(git_dir), 0);
    ASSERT_EQ(th_mkdir_p(nested_dir), 0);
    ASSERT_EQ(th_write_file(marker,
                            "{\"version\":1,\"remote_url\":\"git@example.com:team/repo.git\","
                            "\"branch\":\"main\",\"poll_interval_sec\":300}"),
              0);
    ASSERT_EQ(th_write_file(nested_file, "managed clone"), 0);
    ASSERT_EQ(ui_delete_make_project_db(&fx, project, managed_root), 0);
    cbm_watcher_watch(fx.watcher, project, managed_root);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-remote", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_FALSE(cbm_is_dir(managed_root));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 0);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_unwatches_missing_db) {
    /* A missing database already satisfies DELETE and is therefore successful. */
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    cbm_watcher_watch(fx.watcher, "ui-delete-missing", fx.root_dir);
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-missing", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"not_found\""));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 0);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_no_watcher_still_deletes) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    ASSERT_EQ(ui_delete_make_db_file(&fx, "ui-delete-no-watcher"), 0);

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-no-watcher", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 200);

    char db[1024];
    ui_delete_db_path(&fx, "ui-delete-no-watcher", db, sizeof(db));
    ASSERT_FALSE(cbm_file_exists(db));

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_missing_name_keeps_watch) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    cbm_watcher_watch(fx.watcher, "ui-delete-still-watched", fx.root_dir);
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 400);
    ASSERT_NOT_NULL(strstr(resp, "{\"error\":\"missing name\"}"));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_invalid_name_keeps_watch) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    cbm_watcher_watch(fx.watcher, "bad/name", fx.root_dir);
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=bad%2Fname", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 400);
    ASSERT_NOT_NULL(strstr(resp, "{\"error\":\"invalid project name\"}"));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_delete_project_unlink_failure_keeps_watch) {
    ui_delete_fixture_t fx;
    ASSERT_EQ(ui_delete_fixture_init(&fx), 0);
    char db[1024];
    ui_delete_db_path(&fx, "ui-delete-unlink-fails", db, sizeof(db));
    ASSERT_EQ(th_mkdir_p(db), 0);
    cbm_watcher_watch(fx.watcher, "ui-delete-unlink-fails", fx.root_dir);
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_t ts;
    ASSERT_EQ(th_server_start_with_watcher(&ts, fx.watcher), 0);
    char resp[4096];
    int n = ui_delete_request(&ts, "/api/project?name=ui-delete-unlink-fails", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 409);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"delete_failed\""));
    ASSERT_TRUE(cbm_file_exists(db));
    ASSERT_EQ(cbm_watcher_watch_count(fx.watcher), 1);

    th_server_stop(&ts);
    ui_delete_fixture_cleanup(&fx);
    PASS();
}

TEST(ui_server_ui_config_detects_zh_accept_language) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);

    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "GET /api/ui-config HTTP/1.1\r\n"
                    "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
                    "\r\n",
                    resp, sizeof(resp));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"lang\":\"zh\""));

    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_ui_config_prefers_config_lang) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_httpd_cfg_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    cbm_setenv("HOME", td, 1);

    char cache_dir[1024];
    snprintf(cache_dir, sizeof(cache_dir), "%s", cbm_resolve_cache_dir());
    cbm_config_t *cfg = cbm_config_open(cache_dir);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_config_set(cfg, CBM_CONFIG_UI_LANG, "zh"), 0);
    cbm_config_close(cfg);

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);

    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "GET /api/ui-config HTTP/1.1\r\n"
                    "Accept-Language: en-US,en;q=0.9\r\n"
                    "\r\n",
                    resp, sizeof(resp));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"lang\":\"zh\""));

    th_server_stop(&ts);
    if (old_home) {
        cbm_setenv("HOME", old_home, 1);
        free(old_home);
    }
    PASS();
}

TEST(ui_server_slow_request_hits_deadline) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    /* Shorten the deadline so the test is fast */
    cbm_http_server_set_recv_deadline_ms(ts.srv, 300);
    int port = cbm_http_server_port(ts.srv);

    th_sock_t s = th_connect(port);
    ASSERT_TRUE(s != TH_SOCK_BAD);
    ASSERT_EQ(th_send_all(s, "GET /api", 8), 0); /* partial request, then stall */
    char resp[1024];
    int n = th_recv_until_close(s, resp, sizeof(resp)); /* server must give up */
    th_sock_close(s);
    /* Either a 408 or a bare close is acceptable — the loop must move on */
    if (n > 0) {
        ASSERT_EQ(th_status(resp), 408);
    }

    /* …and the server must still answer the next request */
    char resp2[4096];
    int n2 = th_http(port, "GET /definitely/not/here HTTP/1.1\r\n\r\n", resp2, sizeof(resp2));
    ASSERT_GT(n2, 0);
    ASSERT_EQ(th_status(resp2), 404);

    th_server_stop(&ts);
    PASS();
}

TEST(ui_server_access_log_redacts_query) {
    httpd_log_buf[0] = '\0';
    CBMLogLevel prev_level = cbm_log_get_level();
    cbm_log_set_level(CBM_LOG_DEBUG);
    cbm_log_set_format(CBM_LOG_FORMAT_TEXT);
    cbm_log_set_sink_ex(httpd_capture_log, CBM_LOG_SINK_REPLACE);

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    char resp[4096];
    int n = th_http(cbm_http_server_port(ts.srv),
                    "GET /definitely/not/here?token=secret HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 404);
    th_server_stop(&ts);

    cbm_log_set_sink(NULL);
    cbm_log_set_level(prev_level);

    ASSERT_NOT_NULL(strstr(httpd_log_buf, "msg=http.request"));
    ASSERT_NOT_NULL(strstr(httpd_log_buf, "component=graph_ui"));
    ASSERT_NOT_NULL(strstr(httpd_log_buf, "method=GET"));
    ASSERT_NOT_NULL(strstr(httpd_log_buf, "path=/definitely/not/here"));
    ASSERT_NOT_NULL(strstr(httpd_log_buf, "status=404"));
    ASSERT_NULL(strstr(httpd_log_buf, "token"));
    ASSERT_NULL(strstr(httpd_log_buf, "secret"));
    PASS();
}

TEST(ui_server_stop_joins_cleanly) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    /* no requests at all — stop must unblock the accept wait promptly */
    th_server_stop(&ts);
    PASS();
}

/* ── /api/repo-info git-remote URL helpers (distilled from PR #789) ── */

/* The web base must always be https (deep-links can't be downgraded) and must
 * never carry embedded credentials, across scp / ssh / https remote shapes. */
TEST(repo_info_web_base_normalizes_to_https) {
    struct {
        const char *in;
        const char *want;
    } cases[] = {
        {"git@github.com:org/repo.git", "https://github.com/org/repo"},
        {"git@github.com:org/repo", "https://github.com/org/repo"},
        {"https://github.com/org/repo.git", "https://github.com/org/repo"},
        {"ssh://git@github.com/org/repo.git", "https://github.com/org/repo"},
        {"https://user:token@github.com/org/repo.git", "https://github.com/org/repo"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *got = cbm_ui_git_web_base(cases[i].in);
        ASSERT_NOT_NULL(got);
        ASSERT_STR_EQ(got, cases[i].want);
        /* Never leak credentials into the web base. */
        ASSERT_NULL(strstr(got, "token"));
        ASSERT_NULL(strstr(got, "@"));
        free(got);
    }
    /* Unrecognized shapes yield NULL, not a bogus link. */
    ASSERT_NULL(cbm_ui_git_web_base(""));
    ASSERT_NULL(cbm_ui_git_web_base("not-a-url"));
    PASS();
}

/* The remote_url field echoed to the client must have any user:pass@ stripped. */
TEST(repo_info_strips_credentials_from_remote) {
    char *safe = cbm_ui_git_strip_credentials("https://alice:s3cr3t@github.com/org/repo.git");
    ASSERT_NOT_NULL(safe);
    ASSERT_STR_EQ(safe, "https://github.com/org/repo.git");
    ASSERT_NULL(strstr(safe, "s3cr3t"));
    ASSERT_NULL(strstr(safe, "alice"));
    free(safe);

    /* Credential-free URLs pass through unchanged. */
    char *plain = cbm_ui_git_strip_credentials("https://github.com/org/repo.git");
    ASSERT_NOT_NULL(plain);
    ASSERT_STR_EQ(plain, "https://github.com/org/repo.git");
    free(plain);

    /* An '@' in the path (not the authority) must not be treated as creds. */
    char *pathat = cbm_ui_git_strip_credentials("https://github.com/org/repo/@scope");
    ASSERT_NOT_NULL(pathat);
    ASSERT_STR_EQ(pathat, "https://github.com/org/repo/@scope");
    free(pathat);

    /* scp-style carries no secret and is left intact. */
    char *scp = cbm_ui_git_strip_credentials("git@github.com:org/repo.git");
    ASSERT_NOT_NULL(scp);
    ASSERT_STR_EQ(scp, "git@github.com:org/repo.git");
    free(scp);

    ASSERT_NULL(cbm_ui_git_strip_credentials(NULL));
    PASS();
}

/* ── #798 follow-up: full UI-mode hang repro (live sockets) ───── */

/* Like th_http but arms a client-side receive-timeout watchdog. If the
 * single-threaded server wedges, recv() returns instead of blocking forever, so
 * the test FAILs deterministically rather than hanging CI. 0 on connect/timeout. */
static int th_http_deadline(int port, const char *request, char *resp, size_t respsz,
                            int timeout_ms) {
    th_sock_t s = th_connect(port);
    if (s == TH_SOCK_BAD)
        return 0;
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    if (th_send_all(s, request, strlen(request)) != 0) {
        th_sock_close(s);
        return 0;
    }
    int n = th_recv_until_close(s, resp, respsz);
    th_sock_close(s);
    return n;
}

/* #798 was a single-threaded-server wedge: list_projects never returned and the
 * whole UI stopped answering. Assert the running server answers list_projects
 * within a hard deadline while it holds live listening sockets. The client
 * receive-timeout is the watchdog: a wedge → no 200 → FAIL, never a CI hang. */
TEST(ui_server_list_projects_responds_under_watchdog) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}";
    char req[512];
    snprintf(req, sizeof(req),
             "POST /rpc HTTP/1.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    char resp[8192];
    int n = th_http_deadline(cbm_http_server_port(ts.srv), req, resp, sizeof(resp), 15000);
    th_server_stop(&ts);
    ASSERT_GT(n, 0); /* a response arrived before the watchdog fired */
    ASSERT_EQ(th_status(resp), 200);
    ASSERT_NOT_NULL(strstr(resp, "\"jsonrpc\""));
    PASS();
}

#ifdef _WIN32
typedef struct {
    char path[512];
    int resolved_ok;
} th_gitctx_probe_t;

static DWORD WINAPI th_gitctx_probe_thread(LPVOID arg) {
    th_gitctx_probe_t *p = (th_gitctx_probe_t *)arg;
    cbm_git_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = cbm_git_context_resolve(p->path, &ctx);
    p->resolved_ok = (rc == 0 && ctx.is_git) ? 1 : 0;
    cbm_git_context_free(&ctx);
    return 0;
}
#endif

/* The load-bearing end-to-end repro of #798: while the single-threaded UI server
 * holds LIVE listening/AFD socket handles in this process, cbm_git_context_resolve
 * — the exact path list_projects runs (add_git_context_json → resolve →
 * cbm_popen(git)) — must not hang. Under a raw-_popen regression git inherits
 * those sockets and its MSYS2 runtime deadlocks in NtQueryObject; the watchdog
 * turns that into a hard FAIL instead of an infinite hang. */
TEST(git_context_resolve_no_hang_under_live_ui_sockets) {
#ifndef _WIN32
    SKIP_PLATFORM("Windows-only: #798 UI listening-socket handle inheritance");
#else
    char *tmp = th_mktempdir("cbm_798repro");
    if (!tmp)
        FAIL("th_mktempdir returned NULL");

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" init -q && git -C \"%s\" -c user.email=t@t -c user.name=t "
             "commit -q --allow-empty -m init",
             tmp, tmp);
    if (system(cmd) != 0) {
        th_rmtree(tmp);
        SKIP_PLATFORM("git not available to init a repo");
    }

    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);

    th_gitctx_probe_t *probe = (th_gitctx_probe_t *)calloc(1, sizeof(*probe));
    ASSERT_NOT_NULL(probe);
    snprintf(probe->path, sizeof(probe->path), "%s", tmp);

    HANDLE h = CreateThread(NULL, 0, th_gitctx_probe_thread, probe, 0, NULL);
    ASSERT_NOT_NULL(h);
    DWORD w = WaitForSingleObject(h, 30000);
    if (w != WAIT_OBJECT_0) {
        /* Wedged on the inherited-socket NtQueryObject walk. Deliberately leak
         * the heap probe + thread (a late wake must not touch freed memory);
         * process exit reaps them. Fail loudly rather than hang CI. */
        th_server_stop(&ts);
        FAIL("cbm_git_context_resolve hung under live UI sockets (#798 regression)");
    }
    CloseHandle(h);
    th_server_stop(&ts);
    int ok = probe->resolved_ok;
    free(probe);
    th_rmtree(tmp);
    ASSERT_EQ(ok, 1);
    PASS();
#endif
}

/* The server binds to loopback only. A request carrying a non-loopback Host
 * header reached it under a foreign name — the DNS-rebinding / cross-site
 * vector against a localhost service — and must be refused before routing. A
 * loopback Host (or none) proceeds normally. */
TEST(ui_server_rejects_non_loopback_host) {
    th_server_t ts;
    ASSERT_EQ(th_server_start(&ts), 0);
    int port = cbm_http_server_port(ts.srv);
    char resp[4096];

    int n = th_http(port, "GET / HTTP/1.1\r\nHost: evil.example.com\r\n\r\n", resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_EQ(th_status(resp), 403);

    /* A loopback Host is not rejected (routes on to the normal 404 stub). */
    char req[128];
    snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", port);
    n = th_http(port, req, resp, sizeof(resp));
    ASSERT_GT(n, 0);
    ASSERT_NEQ(th_status(resp), 403);

    th_server_stop(&ts);
    PASS();
}

/* The directory browser formats readdir() entries into a fixed 32 KB response
 * buffer. The per-entry loop is clamped, but the trailing "parent"/"roots"
 * appends were not — once the entries filled the buffer, pos ran past the end
 * and the next size argument wrapped, writing out of bounds. Fill the buffer
 * with many long-named subdirectories and browse it in a forked child so an
 * overflow surfaces as a killing signal (ASan abort) rather than a clean run. */
TEST(ui_server_browse_wide_dir_no_overflow) {
#ifdef _WIN32
    SKIP_PLATFORM("fork crash-isolation is POSIX-only; the clamp is platform-agnostic");
#else
    char *dir = th_mktempdir("cbm_browse");
    if (!dir) {
        FAIL("mktempdir");
    }
    char longname[240];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    for (int i = 0; i < 250; i++) { /* 250 * ~220 chars overflows the 32 KB buffer */
        char sub[600];
        snprintf(sub, sizeof(sub), "%s/%s%03d", dir, longname, i);
        th_mkdir_p(sub);
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        th_server_t ts;
        if (th_server_start(&ts) != 0) {
            _exit(2);
        }
        char req[512];
        snprintf(req, sizeof(req), "GET /api/browse?path=%s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                 dir);
        char *resp = malloc(262144);
        int n = resp ? th_http(cbm_http_server_port(ts.srv), req, resp, 262144) : 0;
        int ok = (n > 0 && strstr(resp, "HTTP/1.1 200") != NULL);
        free(resp);
        th_server_stop(&ts);
        _exit(ok ? 0 : 3);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    char rm[600];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", dir);
    (void)system(rm);
    if (WIFSIGNALED(status)) {
        char m[96];
        snprintf(m, sizeof(m), "browse killed by signal %d — response buffer overflow",
                 WTERMSIG(status));
        FAIL(m);
    }
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    PASS();
#endif
}

/* ── Suite ────────────────────────────────────────────────────── */

TEST(ui_server_logs_escape_dense_no_overflow) {
#ifdef _WIN32
    SKIP_PLATFORM("fork crash-isolation is POSIX-only; the clamp is platform-agnostic");
#else
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        char dense[512];
        for (size_t i = 0; i < sizeof(dense) - 1; i++) {
            dense[i] = (i % 2 == 0) ? '"' : '\\';
        }
        dense[sizeof(dense) - 1] = '\0';
        for (int i = 0; i < 500; i++) {
            cbm_ui_log_append(dense);
        }
        th_server_t ts;
        if (th_server_start(&ts) != 0) {
            _exit(2);
        }
        char request[256];
        int port = cbm_http_server_port(ts.srv);
        snprintf(request, sizeof(request),
                 "GET /api/logs?lines=500 HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", port);
        size_t cap = 4u * 1024u * 1024u;
        char *response = malloc(cap);
        int received = response ? th_http(port, request, response, (int)cap) : 0;
        int ok = received > 0 && strstr(response, "HTTP/1.1 200") != NULL;
        free(response);
        th_server_stop(&ts);
        _exit(ok ? 0 : 3);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    PASS();
#endif
}

SUITE(httpd) {
    RUN_TEST(ui_server_browse_wide_dir_no_overflow);
    RUN_TEST(ui_server_logs_escape_dense_no_overflow);
    /* Parser / helpers */
    RUN_TEST(httpd_parse_simple_get);
    RUN_TEST(httpd_parse_post_with_body_offset);
    RUN_TEST(httpd_parse_origin_case_insensitive);
    RUN_TEST(httpd_parse_rejects_bare_lf);
    RUN_TEST(httpd_parse_rejects_chunked);
    RUN_TEST(httpd_parse_rejects_oversized_content_length);
    RUN_TEST(httpd_parse_rejects_garbage_content_length);
    RUN_TEST(httpd_parse_rejects_percent00_in_target);
    RUN_TEST(httpd_parse_rejects_raw_nul_in_head);
    RUN_TEST(httpd_parse_incomplete_head_needs_more);
    RUN_TEST(httpd_parse_rejects_missing_version);
    RUN_TEST(httpd_parse_rejects_oversized_head);
    RUN_TEST(httpd_query_param_decode);
    RUN_TEST(httpd_query_param_edge_cases);
    RUN_TEST(httpd_path_match_matrix);
    RUN_TEST(httpd_resolves_bare_binary_path_from_path);
    RUN_TEST(repo_info_web_base_normalizes_to_https);
    RUN_TEST(repo_info_strips_credentials_from_remote);

    /* Transport */
    RUN_TEST(httpd_listen_ephemeral_port);
    RUN_TEST(httpd_listen_port_collision_returns_null);

    /* Full UI server */
    RUN_TEST(ui_server_rejects_non_loopback_host);
    RUN_TEST(ui_server_unknown_path_404);
    RUN_TEST(ui_server_admin_probes_and_jobs);
    RUN_TEST(ui_server_root_serves_stub_404);
    RUN_TEST(ui_server_cors_localhost_reflected);
    RUN_TEST(ui_server_cors_evil_origin_not_reflected);
    RUN_TEST(ui_server_integrations_returns_read_only_plan);
    RUN_TEST(ui_server_integrations_apply_requires_explicit_confirmation);
    RUN_TEST(ui_server_rpc_initialize);
    RUN_TEST(ui_server_oversized_body_rejected);
    RUN_TEST(ui_server_encoded_slash_not_routed);
    RUN_TEST(ui_server_nul_in_target_rejected);
    RUN_TEST(ui_server_layout_returns_two_linked_projects_with_renderable_cross_edges);
    RUN_TEST(ui_server_browse_traversal_probe);
    RUN_TEST(ui_server_browse_utf8_directory);
    RUN_TEST(ui_server_project_update_requires_project_name);
    RUN_TEST(ui_server_project_update_rejects_unknown_project);
    RUN_TEST(ui_server_delete_project_unwatches_after_delete);
    RUN_TEST(ui_server_delete_project_unicode_cache_path);
    RUN_TEST(ui_server_delete_project_preserves_local_source);
    RUN_TEST(ui_server_delete_project_removes_managed_clone);
    RUN_TEST(ui_server_delete_project_unwatches_missing_db);
    RUN_TEST(ui_server_delete_project_no_watcher_still_deletes);
    RUN_TEST(ui_server_delete_project_missing_name_keeps_watch);
    RUN_TEST(ui_server_delete_project_invalid_name_keeps_watch);
    RUN_TEST(ui_server_delete_project_unlink_failure_keeps_watch);
    RUN_TEST(ui_server_ui_config_detects_zh_accept_language);
    RUN_TEST(ui_server_ui_config_prefers_config_lang);
    RUN_TEST(ui_server_slow_request_hits_deadline);
    RUN_TEST(ui_server_access_log_redacts_query);
    RUN_TEST(ui_server_stop_joins_cleanly);
    /* #798 follow-up: full UI-mode hang repro under live sockets */
    RUN_TEST(ui_server_list_projects_responds_under_watchdog);
    RUN_TEST(git_context_resolve_no_hang_under_live_ui_sockets);
}
