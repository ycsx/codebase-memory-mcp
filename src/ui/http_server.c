/*
 * http_server.c — Routing + endpoint handlers for the graph UI.
 *
 * Transport (sockets, parsing, limits) lives in httpd.c; this file owns
 * the routes and their handlers:
 *   GET /             → embedded index.html
 *   GET /assets/...   → embedded JS/CSS
 *   POST /rpc         → JSON-RPC dispatch via own cbm_mcp_server_t
 *   OPTIONS /rpc      → CORS preflight (for vite dev on :5173)
 *   GET/POST /api/... → UI support endpoints (layout, index, browse, …)
 *   *                 → 404
 *
 * Runs in a background pthread. Binds to 127.0.0.1 only (see httpd.c).
 * Has its own cbm_mcp_server_t with a separate SQLite connection (WAL reader).
 */
#include "ui/http_server.h"
#include "ui/httpd.h"
#include "ui/embedded_assets.h"
#include "ui/layout3d.h"
#include "mcp/mcp.h"
#include "mcp/usage_stats.h"
#include "store/store.h"
#include "watcher/watcher.h"
#include "cli/cli.h"
#include "git/git_context.h"
#include "git/remote_repo.h"

#if defined(HAVE_LIBGIT2)
#include <git2.h> /* git_repository_open, git_remote_lookup, git_remote_url */
#endif
/* pipeline.h no longer needed — indexing runs as subprocess */
#include "foundation/log.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/str_util.h"
#include "foundation/compat_thread.h"
#include "foundation/subprocess.h" /* cbm_build_win_cmdline — shared MS-CRT arg quoting */
#include "foundation/win_utf8.h"   /* cbm_utf8_to_wide — CreateProcessW wide cmdline (#423/#20) */
#include "foundation/win_process.h"
#include "foundation/workspace.h"

#include <sqlite3/sqlite3.h>
#include <yyjson/yyjson.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <psapi.h> /* GetProcessMemoryInfo */
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static const char *desktop_service_token(void) {
    const char *token = getenv("CBM_DESKTOP_SERVICE_TOKEN");
    if (!token || strlen(token) < 48U || strlen(token) > 128U) {
        return "";
    }
    for (const char *p = token; *p; p++) {
        if (!(('0' <= *p && *p <= '9') || ('a' <= *p && *p <= 'f') || ('A' <= *p && *p <= 'F'))) {
            return "";
        }
    }
    return token;
}

/* ── Constants ────────────────────────────────────────────────── */

/* Max JSON-RPC request body size (1 MB) — transport enforces the same cap. */
#define MAX_BODY_SIZE CBM_HTTP_MAX_BODY

/* ── CORS: only allow localhost origins (blocks remote website attacks) ────── */

/* Per-request CORS header buffers. Updated at the start of each dispatch.
 * The server handles requests sequentially on one thread (see httpd.h),
 * which makes these statics safe. */
static char g_cors[256];      /* CORS headers only */
static char g_cors_json[512]; /* CORS + Content-Type: application/json */

/* Inspect the Origin header and only reflect it if it's a localhost URL.
 * This prevents remote websites from making cross-origin requests to the
 * local graph-ui server (the key defense against CORS-based data exfil). */
static void update_cors(const cbm_http_req_t *req) {
    if (req->origin[0] != '\0' && (cbm_http_path_match(req->origin, "http://localhost:*") ||
                                   cbm_http_path_match(req->origin, "http://127.0.0.1:*"))) {
        snprintf(g_cors, sizeof(g_cors),
                 "Access-Control-Allow-Origin: %s\r\n"
                 "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n",
                 req->origin);
    } else {
        /* No Access-Control-Allow-Origin → browser blocks cross-origin access */
        snprintf(g_cors, sizeof(g_cors),
                 "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n");
    }
    snprintf(g_cors_json, sizeof(g_cors_json), "%sContent-Type: application/json\r\n", g_cors);
}

static const char *detect_ui_lang(const char *accept_language) {
    if (accept_language && (strstr(accept_language, "zh-CN") || strstr(accept_language, "zh"))) {
        return "zh";
    }
    return "en";
}

static void handle_ui_config(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    const char *lang = NULL;
    char cache_dir[1024];
    snprintf(cache_dir, sizeof(cache_dir), "%s", cbm_resolve_cache_dir());
    cbm_config_t *cfg = cbm_config_open(cache_dir);
    if (cfg) {
        const char *pinned = cbm_config_get(cfg, CBM_CONFIG_UI_LANG, "auto");
        if (strcmp(pinned, "zh") == 0 || strcmp(pinned, "en") == 0) {
            lang = pinned;
        }
    }

    char lang_buf[8];
    snprintf(lang_buf, sizeof(lang_buf), "%s", lang ? lang : detect_ui_lang(req->accept_language));
    if (cfg) {
        cbm_config_close(cfg);
    }
    /* upstream_issues_url: where the missed-coverage callout (#963) sends
     * edge-case reports. Served from the backend on purpose — the UI security
     * audit forbids hardcoded external URLs in graph-ui source (external
     * targets must come from an auditable backend response, same pattern as
     * the /api/repo-info deep-links). */
    cbm_http_replyf(c, 200, g_cors_json, "{\"lang\":\"%s\",\"upstream_issues_url\":\"%s\"}",
                    lang_buf, "https://github.com/ycsx/codebase-memory-mcp/issues/new");
}

/* ── Server state ─────────────────────────────────────────────── */

struct cbm_http_server {
    cbm_httpd_t *listener;
    cbm_mcp_server_t *mcp;       /* own MCP server instance (read-only) */
    struct cbm_watcher *watcher; /* external watcher ref (not owned) */
    atomic_int stop_flag;
    int port;
    bool listener_ok;
};

/* ── Forward declarations for process-kill PID validation ──────── */

#define MAX_INDEX_JOBS 4

typedef struct {
    unsigned long long job_id;
    char root_path[1024];
    char project_name[256];
    char remote_url[CBM_REMOTE_URL_MAX];
    char remote_branch[CBM_REMOTE_BRANCH_MAX];
    int poll_interval_sec;
    struct cbm_watcher *watcher;
    atomic_int status; /* 0=idle, 1=running, 2=done, 3=error */
    char error_msg[256];
    cbm_thread_t thread;
    bool thread_valid;
    bool reaping;
    long child_pid; /* tracked for process-kill validation */
} index_job_t;

static index_job_t g_index_jobs[MAX_INDEX_JOBS];
static atomic_ullong g_next_index_job_id = 1;
static cbm_mutex_t g_index_jobs_mutex;
static atomic_int g_index_jobs_mutex_init = 0;

typedef struct {
    unsigned long long job_id;
    char root_path[1024];
    char project_name[256];
    char remote_url[CBM_REMOTE_URL_MAX];
    char remote_branch[CBM_REMOTE_BRANCH_MAX];
    int poll_interval_sec;
    struct cbm_watcher *watcher;
} index_job_snapshot_t;

static void index_jobs_init(void) {
    int state = atomic_load(&g_index_jobs_mutex_init);
    if (state == 2)
        return;
    state = 0;
    if (atomic_compare_exchange_strong(&g_index_jobs_mutex_init, &state, 1)) {
        cbm_mutex_init(&g_index_jobs_mutex);
        atomic_store(&g_index_jobs_mutex_init, 2);
        return;
    }
    while (atomic_load(&g_index_jobs_mutex_init) != 2) {
        cbm_usleep(1000);
    }
}

static bool any_index_job_running(void) {
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    bool running = false;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load(&g_index_jobs[i].status) == 1) {
            running = true;
            break;
        }
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
    return running;
}

static bool index_job_running_for_project(const char *project) {
    if (!project || !project[0]) {
        return false;
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    bool running = false;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load(&g_index_jobs[i].status) == 1 &&
            strcmp(g_index_jobs[i].project_name, project) == 0) {
            running = true;
            break;
        }
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
    return running;
}

static void set_job_status(index_job_t *job, int status) {
    if (!job) {
        return;
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    atomic_store(&job->status, status);
    cbm_mutex_unlock(&g_index_jobs_mutex);
}

static void snapshot_index_job(index_job_t *job, index_job_snapshot_t *out) {
    if (!job || !out) {
        return;
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    out->job_id = job->job_id;
    snprintf(out->root_path, sizeof(out->root_path), "%s", job->root_path);
    snprintf(out->project_name, sizeof(out->project_name), "%s", job->project_name);
    snprintf(out->remote_url, sizeof(out->remote_url), "%s", job->remote_url);
    snprintf(out->remote_branch, sizeof(out->remote_branch), "%s", job->remote_branch);
    out->poll_interval_sec = job->poll_interval_sec;
    out->watcher = job->watcher;
    cbm_mutex_unlock(&g_index_jobs_mutex);
}

/* Graph UI tool calls carry the selected project in params.arguments. While an
 * index worker replaces project A's database, keep A closed on Windows but do
 * not reject independent reads for projects B, C, and so on. Calls without an
 * explicit project remain conservatively blocked because their store scope
 * cannot be proven safe. */
static bool rpc_targets_running_project(const char *body, size_t body_len) {
    if (!any_index_job_running() || !body || body_len == 0) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *method = root ? yyjson_obj_get(root, "method") : NULL;
    if (!method || !yyjson_is_str(method) || strcmp(yyjson_get_str(method), "tools/call") != 0) {
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_val *params = yyjson_obj_get(root, "params");
    yyjson_val *arguments =
        params && yyjson_is_obj(params) ? yyjson_obj_get(params, "arguments") : NULL;
    static const char *const project_keys[] = {"project", "project_name", "project_id",
                                               "projectName"};
    const char *project = NULL;
    if (arguments && yyjson_is_obj(arguments)) {
        for (size_t i = 0; i < sizeof(project_keys) / sizeof(project_keys[0]); i++) {
            yyjson_val *value = yyjson_obj_get(arguments, project_keys[i]);
            if (value && yyjson_is_str(value) && yyjson_get_str(value)[0]) {
                project = yyjson_get_str(value);
                break;
            }
        }
    }

    bool conflicts = project ? index_job_running_for_project(project) : true;
    yyjson_doc_free(doc);
    return conflicts;
}

/* ── Serve embedded asset ─────────────────────────────────────── */

/* Content-Security-Policy for the served UI. No external host appears in any
 * directive, so the browser cannot load or connect to anything off-origin —
 * this ENFORCES the airgap (the code makes no external calls; this stops a
 * future dependency or injected content from doing so). connect-src 'self'
 * confines fetch/XHR/WebSocket to the local server. The 'self'/data:/blob:/
 * 'unsafe-inline'-style/'wasm-unsafe-eval' allowances cover the bundled app's
 * own needs (React inline styles, three.js textures/workers/WASM). */
#define CBM_UI_CSP                                                       \
    "Content-Security-Policy: default-src 'self'; connect-src 'self'; "  \
    "img-src 'self' data: blob:; script-src 'self' 'wasm-unsafe-eval'; " \
    "style-src 'self' 'unsafe-inline'; font-src 'self' data:; "          \
    "worker-src 'self' blob:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n"

static bool serve_embedded(cbm_http_conn_t *c, const char *path) {
    const cbm_embedded_file_t *f = cbm_embedded_lookup(path);
    if (!f)
        return false;

    /* Build headers with correct Content-Type for this asset */
    char hdrs[1024];
    snprintf(hdrs, sizeof(hdrs),
             "%sContent-Type: %s\r\n"
             "Cache-Control: public, max-age=31536000, immutable\r\n" CBM_UI_CSP,
             g_cors, f->content_type);

    cbm_http_reply_buf(c, 200, hdrs, f->data, (size_t)f->size);
    return true;
}

/* Build DB path for a project: <cache_dir>/<project>.db */
static void db_path_for_project(const char *project, char *buf, size_t bufsz) {
    if (!cbm_validate_project_name(project)) {
        buf[0] = '\0';
        return;
    }
    const char *dir = cbm_resolve_cache_dir();
    if (!dir) {
        dir = cbm_tmpdir();
    }
    snprintf(buf, bufsz, "%s/%s.db", dir, project);
}

/* ── Git remote → GitHub deep-link base (/api/repo-info) ───────── */

/* Return a copy of `url` with any "user[:password]@" userinfo removed from the
 * scheme://authority form, so credentials are never echoed back to the client.
 * scp-style (git@host:path) is returned unchanged: "git" there is a login name,
 * not a secret. malloc'd copy, or NULL when url is NULL. Caller frees. */
char *cbm_ui_git_strip_credentials(const char *url) {
    if (!url)
        return NULL;
    const char *sep = strstr(url, "://");
    if (!sep)
        return strdup(url); /* scp-style / opaque — no scheme userinfo to strip */
    const char *authority = sep + 3;
    const char *slash = strchr(authority, '/');
    const char *at = strchr(authority, '@');
    if (!at || (slash && at > slash))
        return strdup(url); /* '@' is in the path, not the authority → no creds */
    size_t prefix = (size_t)(authority - url); /* "scheme://" */
    const char *rest = at + 1;
    size_t out_len = prefix + strlen(rest) + 1;
    char *out = malloc(out_len);
    if (!out)
        return NULL;
    memcpy(out, url, prefix);
    memcpy(out + prefix, rest, strlen(rest) + 1);
    return out;
}

/* Normalize a git remote URL (scp-style, ssh://, https://) to a canonical
 * "https://host/org/repo" web base with any trailing ".git" and any embedded
 * credentials removed. Returns a malloc'd string or NULL if the shape isn't
 * recognized. Caller frees. */
char *cbm_ui_git_web_base(const char *url) {
    if (!url || !url[0])
        return NULL;
    char host_path[1024] = {0}; /* "host/org/repo" */
    if (strncmp(url, "git@", 4) == 0) {
        const char *at = url + 4;
        const char *colon = strchr(at, ':');
        if (!colon)
            return NULL;
        snprintf(host_path, sizeof(host_path), "%.*s/%s", (int)(colon - at), at, colon + 1);
    } else {
        const char *p = strstr(url, "://");
        if (!p)
            return NULL;
        p += 3;
        const char *at = strchr(p, '@'); /* strip any embedded credentials */
        if (at)
            p = at + 1;
        snprintf(host_path, sizeof(host_path), "%s", p);
    }
    size_t l = strlen(host_path);
    if (l > 4 && strcmp(host_path + l - 4, ".git") == 0)
        host_path[l - 4] = '\0';
    l = strlen(host_path);
    if (l > 0 && host_path[l - 1] == '/')
        host_path[l - 1] = '\0';
    size_t out_sz = strlen(host_path) + 9; /* "https://" (8) + NUL */
    char *out = malloc(out_sz);
    if (!out)
        return NULL;
    /* Legitimate GitHub blob-URL construction, not a network call — the scheme
     * is https-forced here so the frontend deep-link can never be downgraded.
     * Allow-listed in scripts/security-allowlist.txt (URL:https://%s). */
    snprintf(out, out_sz, "https://%s", host_path);
    return out;
}

/* Read the "origin" remote URL for the repo at root_path. malloc'd or NULL.
 * libgit2 is initialized once at process start by cbm_alloc_init() (which also
 * binds its allocator to mimalloc) — do NOT git_libgit2_init()/shutdown() here:
 * a per-request shutdown could drop the global refcount and tear down that
 * allocator binding mid-process. */
static char *git_origin_remote_url(const char *root_path) {
#if defined(HAVE_LIBGIT2)
    git_repository *repo = NULL;
    char *out = NULL;
    if (git_repository_open(&repo, root_path) == 0) {
        git_remote *rem = NULL;
        if (git_remote_lookup(&rem, repo, "origin") == 0) {
            const char *u = git_remote_url(rem);
            if (u)
                out = strdup(u);
            git_remote_free(rem);
        }
        git_repository_free(repo);
    }
    return out;
#else
    (void)root_path;
    return NULL;
#endif
}

/* GET /api/repo-info?project=NAME → { root_path, branch, remote_url, web_base,
 * blob_base }. blob_base is "<web_base>/blob/<branch>" ready for the frontend to
 * append "/<file_path>#L<start>-L<end>". remote_url is credential-stripped;
 * fields are empty strings when unknown (e.g. no git remote). */
static void handle_repo_info(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char project[256] = {0};
    if (!cbm_http_query_param(req->query, "project", project, (int)sizeof(project)) ||
        project[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project parameter\"}");
        return;
    }

    char db_path[1024];
    db_path_for_project(project, db_path, sizeof(db_path));
    if (db_path[0] == '\0' || !cbm_file_exists(db_path)) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot open store\"}");
        return;
    }

    char root_path[1024] = {0};
    cbm_project_t proj;
    memset(&proj, 0, sizeof(proj));
    if (cbm_store_get_project(store, project, &proj) == CBM_STORE_OK && proj.root_path) {
        snprintf(root_path, sizeof(root_path), "%s", proj.root_path);
    }
    cbm_project_free_fields(&proj);
    cbm_store_close(store);

    char branch[256] = {0};
    if (root_path[0]) {
        cbm_git_context_t gctx;
        memset(&gctx, 0, sizeof(gctx));
        if (cbm_git_context_resolve(root_path, &gctx) == 0 && gctx.branch) {
            snprintf(branch, sizeof(branch), "%s", gctx.branch);
        }
        cbm_git_context_free(&gctx);
    }

    char *remote = root_path[0] ? git_origin_remote_url(root_path) : NULL;
    char *remote_safe = cbm_ui_git_strip_credentials(remote); /* never echo secrets */
    char *web_base = cbm_ui_git_web_base(remote);

    char blob_base[1152] = {0};
    if (web_base && web_base[0] && branch[0]) {
        snprintf(blob_base, sizeof(blob_base), "%s/blob/%s", web_base, branch);
    }

    /* JSON-escape the free-form fields. */
    char esc_root[2048], esc_branch[512], esc_remote[2048], esc_web[2048], esc_blob[2304];
    cbm_json_escape(esc_root, (int)sizeof(esc_root), root_path);
    cbm_json_escape(esc_branch, (int)sizeof(esc_branch), branch);
    cbm_json_escape(esc_remote, (int)sizeof(esc_remote), remote_safe ? remote_safe : "");
    cbm_json_escape(esc_web, (int)sizeof(esc_web), web_base ? web_base : "");
    cbm_json_escape(esc_blob, (int)sizeof(esc_blob), blob_base);

    cbm_http_replyf(c, 200, g_cors_json,
                    "{\"root_path\":\"%s\",\"branch\":\"%s\",\"remote_url\":\"%s\","
                    "\"web_base\":\"%s\",\"blob_base\":\"%s\"}",
                    esc_root, esc_branch, esc_remote, esc_web, esc_blob);

    free(remote);
    free(remote_safe);
    free(web_base);
}

/* ── Log ring buffer ──────────────────────────────────────────── */

#define LOG_RING_SIZE 500
#define LOG_LINE_MAX 512

static char g_log_ring[LOG_RING_SIZE][LOG_LINE_MAX];
static int g_log_head = 0;
static int g_log_count = 0;
static cbm_mutex_t g_log_mutex;

enum { CBM_LOG_MUTEX_UNINIT = 0, CBM_LOG_MUTEX_INITING = 1, CBM_LOG_MUTEX_INITED = 2 };
static atomic_int g_log_mutex_init = CBM_LOG_MUTEX_UNINIT;

/* Safe for concurrent callers: only publishes INITED after cbm_mutex_init()
 * has completed. Callers that lose the CAS race spin until init finishes. */
void cbm_ui_log_init(void) {
    int state = atomic_load(&g_log_mutex_init);
    if (state == CBM_LOG_MUTEX_INITED)
        return;

    state = CBM_LOG_MUTEX_UNINIT;
    if (atomic_compare_exchange_strong(&g_log_mutex_init, &state, CBM_LOG_MUTEX_INITING)) {
        cbm_mutex_init(&g_log_mutex);
        atomic_store(&g_log_mutex_init, CBM_LOG_MUTEX_INITED);
        return;
    }

    /* Another thread is initializing — spin until done */
    while (atomic_load(&g_log_mutex_init) != CBM_LOG_MUTEX_INITED) {
        cbm_usleep(1000); /* 1ms */
    }
}

/* Called from a log hook — appends a line to the ring buffer (thread-safe) */
void cbm_ui_log_append(const char *line) {
    if (!line)
        return;
    /* Ensure mutex is initialized (safe for early single-threaded logging
     * and concurrent calls via atomic_exchange once-init pattern). */
    cbm_ui_log_init();
    cbm_mutex_lock(&g_log_mutex);
    snprintf(g_log_ring[g_log_head], LOG_LINE_MAX, "%s", line);
    g_log_head = (g_log_head + 1) % LOG_RING_SIZE;
    if (g_log_count < LOG_RING_SIZE)
        g_log_count++;
    cbm_mutex_unlock(&g_log_mutex);
}

/* Append a printf-formatted fragment at *pos within a bufsz buffer, never
 * advancing *pos past bufsz. snprintf returns the length it WOULD have written,
 * so `pos += snprintf(...)` runs pos past the end on truncation and the next
 * call computes a wrapped (huge) remaining size and writes out of bounds. This
 * clamps: on truncation *pos is pinned at bufsz and further appends are no-ops. */
static void http_appendf(char *buf, size_t bufsz, int *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void http_appendf(char *buf, size_t bufsz, int *pos, const char *fmt, ...) {
    if (*pos < 0) {
        return;
    }
    if ((size_t)*pos >= bufsz) {
        *pos = (int)bufsz;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, bufsz - (size_t)*pos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= bufsz - (size_t)*pos) {
        *pos = (int)bufsz;
    } else {
        *pos += n;
    }
}

/* GET /api/logs?lines=N — returns last N log lines */
static void handle_logs(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char lines_str[16] = {0};
    int max_lines = 100;
    if (cbm_http_query_param(req->query, "lines", lines_str, (int)sizeof(lines_str))) {
        int v = atoi(lines_str);
        if (v > 0 && v <= LOG_RING_SIZE)
            max_lines = v;
    }

    cbm_mutex_lock(&g_log_mutex);
    int count = g_log_count < max_lines ? g_log_count : max_lines;
    int start = (g_log_head - count + LOG_RING_SIZE) % LOG_RING_SIZE;
    int total = g_log_count;

    /* JSON escaping can double every stored byte. */
    size_t buf_size = (size_t)count * (2 * LOG_LINE_MAX + 8) + 64;
    char *buf = malloc(buf_size);
    if (!buf) {
        cbm_mutex_unlock(&g_log_mutex);
        cbm_http_replyf(c, 500, g_cors, "oom");
        return;
    }

    int pos = 0;
    http_appendf(buf, buf_size, &pos, "{\"lines\":[");
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        if (i > 0)
            http_appendf(buf, buf_size, &pos, ",");
        /* Escape quotes in log lines */
        http_appendf(buf, buf_size, &pos, "\"");
        for (int j = 0; g_log_ring[idx][j] && (size_t)pos < buf_size - 10; j++) {
            char ch = g_log_ring[idx][j];
            if (ch == '"') {
                buf[pos++] = '\\';
                buf[pos++] = '"';
            } else if (ch == '\\') {
                buf[pos++] = '\\';
                buf[pos++] = '\\';
            } else if (ch == '\n') {
                buf[pos++] = '\\';
                buf[pos++] = 'n';
            } else {
                buf[pos++] = ch;
            }
        }
        http_appendf(buf, buf_size, &pos, "\"");
    }
    cbm_mutex_unlock(&g_log_mutex);
    http_appendf(buf, buf_size, &pos, "],\"total\":%d}", total);

    if ((size_t)pos >= buf_size) {
        pos = (int)buf_size - 1;
    }
    buf[pos] = '\0';

    cbm_http_replyf(c, 200, g_cors_json, "%s", buf);
    free(buf);
}

/* GET /api/usage-stats?limit=N — initialized AI MCP tool calls, aggregated by target file. */
static void handle_usage_stats(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char limit_str[16] = {0};
    int limit = 12;
    if (cbm_http_query_param(req->query, "limit", limit_str, (int)sizeof(limit_str))) {
        int parsed = atoi(limit_str);
        if (parsed > 0 && parsed <= 100) {
            limit = parsed;
        }
    }

    char *json = cbm_usage_stats_json(limit);
    if (!json) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"usage stats unavailable\"}");
        return;
    }
    cbm_http_replyf(c, 200, g_cors_json, "%s", json);
    free(json);
}

/* ── Process monitoring ───────────────────────────────────────── */

#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <signal.h>

/* GET /api/processes — list codebase-memory-mcp processes via ps */
static void handle_processes(cbm_http_conn_t *c) {
    char buf[8192];
    int pos = 0;

#ifdef _WIN32
    /* Windows: GetProcessMemoryInfo + GetProcessTimes */
    PROCESS_MEMORY_COUNTERS pmc;
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    double user_s = 0, sys_s = 0;
    size_t rss_bytes = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        rss_bytes = pmc.WorkingSetSize;
    if (GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
        ULARGE_INTEGER u, k;
        u.LowPart = ft_user.dwLowDateTime;
        u.HighPart = ft_user.dwHighDateTime;
        k.LowPart = ft_kernel.dwLowDateTime;
        k.HighPart = ft_kernel.dwHighDateTime;
        user_s = (double)u.QuadPart / 1e7;
        sys_s = (double)k.QuadPart / 1e7;
    }
    const char *service_token = desktop_service_token();
    http_appendf(buf, sizeof(buf), &pos,
                 "{\"self_pid\":%d,\"self_rss_mb\":%.1f,"
                 "\"self_user_cpu_s\":%.1f,\"self_sys_cpu_s\":%.1f,"
                 "\"service_token\":\"%s\",\"processes\":[",
                 (int)_getpid(), (double)rss_bytes / (1024.0 * 1024.0), user_s, sys_s,
                 service_token);
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    bool first_process = true;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load(&g_index_jobs[i].status) != 1 || g_index_jobs[i].child_pid <= 0) {
            continue;
        }
        if (!first_process) {
            http_appendf(buf, sizeof(buf), &pos, ",");
        }
        first_process = false;
        http_appendf(buf, sizeof(buf), &pos,
                     "{\"pid\":%ld,\"cpu\":0.0,\"rss_mb\":0.0,"
                     "\"elapsed\":\"\",\"command\":\"index-worker\",\"is_self\":false}",
                     g_index_jobs[i].child_pid);
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
    http_appendf(buf, sizeof(buf), &pos, "]}");
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long rss_kb = ru.ru_maxrss;
#ifdef __APPLE__
    rss_kb /= 1024;
#endif
    http_appendf(buf, sizeof(buf), &pos,
                 "{\"self_pid\":%d,\"self_rss_mb\":%.1f,"
                 "\"self_user_cpu_s\":%.1f,\"self_sys_cpu_s\":%.1f,"
                 "\"service_token\":\"%s\",\"processes\":[",
                 (int)getpid(), (double)rss_kb / 1024.0,
                 (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6,
                 (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6,
                 desktop_service_token());

    FILE *fp = popen("LC_ALL=C ps -eo pid,pcpu,rss,etime,comm 2>/dev/null"
                     " | grep '[c]odebase-memory-mcp'",
                     "r");
    int proc_count = 0;
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            int pid = 0;
            float cpu = 0;
            long rss = 0;
            char elapsed[64] = {0};
            char comm[256] = {0};

            if (sscanf(line, "%d %f %ld %63s %255s", &pid, &cpu, &rss, elapsed, comm) >= 4) {
                if (proc_count > 0)
                    buf[pos++] = ',';
                http_appendf(buf, sizeof(buf), &pos,
                             "{\"pid\":%d,\"cpu\":%.1f,\"rss_mb\":%.1f,"
                             "\"elapsed\":\"%s\",\"command\":\"%s\",\"is_self\":%s}",
                             pid, (double)cpu, (double)rss / 1024.0, elapsed, comm,
                             pid == (int)getpid() ? "true" : "false");
                if (pos >= (int)sizeof(buf)) {
                    pos = (int)sizeof(buf) - 1;
                }
                proc_count++;
            }
        }
        pclose(fp);
    }
    http_appendf(buf, sizeof(buf), &pos, "]}");
#endif

    cbm_http_replyf(c, 200, g_cors_json, "%s", buf);
}

/* POST /api/process-kill — kill a process by PID */
static void handle_process_kill(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 256) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v_pid = yyjson_obj_get(root, "pid");
    if (!v_pid || !yyjson_is_int(v_pid)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing pid\"}");
        return;
    }
    int target_pid = (int)yyjson_get_int(v_pid);
    yyjson_doc_free(doc);

#ifdef _WIN32
    if (target_pid == (int)_getpid()) {
#else
    if (target_pid == (int)getpid()) {
#endif
        cbm_http_replyf(c, 400, g_cors_json,
                        "{\"error\":\"cannot kill self (use the UI server's own shutdown)\"}");
        return;
    }

    /* Only allow killing PIDs that were spawned by this server (indexing jobs). */
    {
        bool pid_is_ours = false;
        index_jobs_init();
        cbm_mutex_lock(&g_index_jobs_mutex);
        for (int i = 0; i < MAX_INDEX_JOBS; i++) {
            if (atomic_load(&g_index_jobs[i].status) == 1 &&
                g_index_jobs[i].child_pid == target_pid) {
                pid_is_ours = true;
                break;
            }
        }
        cbm_mutex_unlock(&g_index_jobs_mutex);
        if (!pid_is_ours) {
            cbm_http_replyf(c, 403, g_cors_json,
                            "{\"error\":\"can only kill server-spawned processes\"}");
            return;
        }
    }

#ifdef _WIN32
    HANDLE hproc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)target_pid);
    if (!hproc || !TerminateProcess(hproc, 1)) {
        if (hproc)
            CloseHandle(hproc);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"kill failed\"}");
        return;
    }
    CloseHandle(hproc);
#else
    if (kill(target_pid, SIGTERM) != 0) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"kill failed\"}");
        return;
    }
#endif

    cbm_http_replyf(c, 200, g_cors_json, "{\"killed\":%d}", target_pid);
}

/* ── Directory browser ────────────────────────────────────────── */

static void append_roots_json(char *buf, size_t bufsz, int *pos) {
    http_appendf(buf, bufsz, pos, ",\"roots\":[");
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    int count = 0;
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) {
            continue;
        }
        if (count++ > 0) {
            http_appendf(buf, bufsz, pos, ",");
        }
        http_appendf(buf, bufsz, pos, "\"%c:/\"", 'A' + i);
    }
#else
    http_appendf(buf, bufsz, pos, "\"/\"");
#endif
    http_appendf(buf, bufsz, pos, "]");
}

/* GET /api/browse?path=/some/dir — list subdirectories for file picker */
static void handle_browse(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char path[1024] = {0};
    const char *home = cbm_get_home_dir();
    if (!cbm_http_query_param(req->query, "path", path, (int)sizeof(path)) || path[0] == '\0') {
        /* Default to home directory */
        if (home)
            snprintf(path, sizeof(path), "%s", home);
        else
            snprintf(path, sizeof(path), "/");
    }

    /* The browser UI may send Windows backslash separators (e.g.
     * "D:\projects\demo"). Normalize to forward slashes before the cbm_is_dir
     * gate, exactly as the MCP repo_path handler and cbm_project_name_from_path
     * already do — otherwise a real D:/ directory is rejected (#548). */
    cbm_normalize_path_sep(path);

    if (!cbm_is_dir(path)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"not a directory\"}");
        return;
    }

    cbm_dir_t *dir = cbm_opendir(path);
    if (!dir) {
        cbm_http_replyf(c, 403, g_cors_json, "{\"error\":\"cannot open directory\"}");
        return;
    }

    /* Build JSON response */
    char buf[32768];
    int pos = 0;
    char escaped_path[2048];
    cbm_json_escape(escaped_path, (int)sizeof(escaped_path), path);
    http_appendf(buf, sizeof(buf), &pos, "{\"path\":\"%s\",\"dirs\":[", escaped_path);

    cbm_dirent_t *ent;
    int count = 0;
    while ((ent = cbm_readdir(dir)) != NULL) {
        /* Skip hidden dirs and . / .. */
        if (ent->name[0] == '.')
            continue;

        if (!ent->is_dir)
            continue;

        if (count > 0)
            buf[pos++] = ',';
        /* Escape directory name to prevent XSS (e.g., names with quotes/angle brackets) */
        {
            char esc[512];
            cbm_json_escape(esc, (int)sizeof(esc), ent->name);
            http_appendf(buf, sizeof(buf), &pos, "\"%s\"", esc);
        }
        if (pos >= (int)sizeof(buf)) {
            pos = (int)sizeof(buf) - 1;
        }
        count++;

        if (count >= 200)
            break; /* safety limit */
    }
    cbm_closedir(dir);

    /* Parent path — escape to prevent injection */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", path);
    char *last_slash = strrchr(parent, '/');
    /* A Windows drive root "X:/" is its own parent (like POSIX "/"): truncating
     * at the slash would yield the bare drive spec "X:", which the next browse
     * resolves to the wrong directory and strands the user at the root (#548). */
    size_t parent_len = strlen(parent);
    bool is_drive_root = parent_len == 3 && parent[1] == ':' && parent[2] == '/';
    if (is_drive_root) {
        /* leave "X:/" unchanged */
    } else if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else {
        snprintf(parent, sizeof(parent), "/");
    }

    {
        char esc_parent[2048];
        cbm_json_escape(esc_parent, (int)sizeof(esc_parent), parent);
        http_appendf(buf, sizeof(buf), &pos, "],\"parent\":\"%s\"", esc_parent);
        append_roots_json(buf, sizeof(buf), &pos);
        http_appendf(buf, sizeof(buf), &pos, "}");
    }
    cbm_http_replyf(c, 200, g_cors_json, "%s", buf);
}

/* ── ADR endpoints ────────────────────────────────────────────── */

/* GET /api/adr?project=X — get ADR content for a project */
static void handle_adr_get(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char name[256] = {0};
    if (!cbm_http_query_param(req->query, "project", name, (int)sizeof(name)) || name[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project\"}");
        return;
    }

    char db_path[1024];
    db_path_for_project(name, db_path, sizeof(db_path));

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"has_adr\":false}");
        return;
    }

    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    if (cbm_store_adr_get(store, name, &adr) == CBM_STORE_OK && adr.content) {
        /* Escape content for JSON — simple: replace quotes and newlines */
        size_t clen = strlen(adr.content);
        size_t buf_size = clen * 2 + 256;
        char *buf = malloc(buf_size);
        if (buf) {
            int pos = snprintf(buf, buf_size, "{\"has_adr\":true,\"content\":\"");
            for (size_t i = 0; i < clen && (size_t)pos < buf_size - 10; i++) {
                char ch = adr.content[i];
                if (ch == '"') {
                    buf[pos++] = '\\';
                    buf[pos++] = '"';
                } else if (ch == '\\') {
                    buf[pos++] = '\\';
                    buf[pos++] = '\\';
                } else if (ch == '\n') {
                    buf[pos++] = '\\';
                    buf[pos++] = 'n';
                } else if (ch == '\r') { /* skip */
                } else if (ch == '\t') {
                    buf[pos++] = '\\';
                    buf[pos++] = 't';
                } else {
                    buf[pos++] = ch;
                }
            }
            http_appendf(buf, buf_size, &pos, "\",\"updated_at\":\"%s\"}",
                         adr.updated_at ? adr.updated_at : "");
            cbm_http_replyf(c, 200, g_cors_json, "%s", buf);
            free(buf);
        } else {
            cbm_http_replyf(c, 500, g_cors, "oom");
        }
        cbm_store_adr_free(&adr);
    } else {
        cbm_http_replyf(c, 200, g_cors_json, "{\"has_adr\":false}");
    }
    cbm_store_close(store);
}

/* POST /api/adr — save ADR content. Body: {"project":"...","content":"..."} */
static void handle_adr_save(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 16384) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v_proj = yyjson_obj_get(root, "project");
    yyjson_val *v_content = yyjson_obj_get(root, "content");
    if (!v_proj || !yyjson_is_str(v_proj) || !v_content || !yyjson_is_str(v_content)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project or content\"}");
        return;
    }

    const char *proj = yyjson_get_str(v_proj);
    const char *content = yyjson_get_str(v_content);

    char db_path[1024];
    db_path_for_project(proj, db_path, sizeof(db_path));

    cbm_store_t *store = cbm_store_open_path(db_path);
    yyjson_doc_free(doc);
    if (!store) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot open store\"}");
        return;
    }

    int rc = cbm_store_adr_store(store, proj, content);
    cbm_store_close(store);

    if (rc == CBM_STORE_OK) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"saved\":true}");
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"save failed\"}");
    }
}

/* ── Background indexing ──────────────────────────────────────── */

static char g_binary_path[1024] = {0};

static char *build_integrations_plan(void) {
    const char *home = cbm_get_home_dir();
    if (!home || !home[0] || !g_binary_path[0]) {
        return NULL;
    }
    return cbm_build_install_plan_json(home, g_binary_path);
}

/* Reuse the CLI dry-run plan so the UI and command line cannot drift on
 * client detection or the files an install would manage. */
static void handle_integrations_get(cbm_http_conn_t *c) {
    char *plan = build_integrations_plan();
    if (!plan) {
        cbm_http_replyf(c, 503, g_cors_json, "%s", "{\"error\":\"integration plan unavailable\"}");
        return;
    }
    cbm_http_replyf(c, 200, g_cors_json, "%s", plan);
    free(plan);
}

static void handle_integrations_apply(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 256) {
        cbm_http_replyf(c, 400, g_cors_json, "%s", "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *confirm = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "confirm") : NULL;
    bool confirmed = confirm && yyjson_is_bool(confirm) && yyjson_get_bool(confirm);
    if (doc) {
        yyjson_doc_free(doc);
    }
    if (!confirmed) {
        cbm_http_replyf(c, 400, g_cors_json, "%s",
                        "{\"error\":\"explicit confirmation required\"}");
        return;
    }

    const char *home = cbm_get_home_dir();
    if (!home || !home[0] || !g_binary_path[0]) {
        cbm_http_replyf(c, 503, g_cors_json, "%s",
                        "{\"error\":\"integration install unavailable\"}");
        return;
    }

    int rc = cbm_install_agent_configs(home, g_binary_path, false, false);
    if (rc != 0) {
        cbm_http_replyf(c, 409, g_cors_json, "%s",
                        "{\"error\":\"one or more client configurations were refused\"}");
        return;
    }

    char *plan = build_integrations_plan();
    if (plan) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"applied\",\"plan\":%s}", plan);
        free(plan);
    } else {
        cbm_http_replyf(c, 200, g_cors_json, "%s", "{\"status\":\"applied\"}");
    }
}

static bool copy_path(char *out, size_t outsz, const char *path) {
    if (!out || outsz == 0 || !path || !path[0]) {
        return false;
    }
    int n = snprintf(out, outsz, "%s", path);
    return n > 0 && (size_t)n < outsz;
}

#ifndef _WIN32
static bool is_executable_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static bool resolve_from_path(const char *name, char *out, size_t outsz) {
    const char *path = getenv("PATH");
    if (!name || !name[0] || strchr(name, '/') || !path || !path[0]) {
        return false;
    }

    const char *cur = path;
    while (*cur) {
        const char *colon = strchr(cur, ':');
        size_t dir_len = colon ? (size_t)(colon - cur) : strlen(cur);
        if (dir_len > 0 && dir_len < 900) {
            char candidate[1024];
            int n = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)dir_len, cur, name);
            if (n > 0 && (size_t)n < sizeof(candidate) && is_executable_file(candidate)) {
                return copy_path(out, outsz, candidate);
            }
        }
        if (!colon) {
            break;
        }
        cur = colon + 1;
    }
    return false;
}

static bool resolve_self_executable(char *out, size_t outsz) {
#if defined(__APPLE__)
    char buf[1024];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0 && buf[0]) {
        return copy_path(out, outsz, buf);
    }
    return false;
#else
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return copy_path(out, outsz, buf);
    }
    return false;
#endif
}
#else
static bool resolve_self_executable(char *out, size_t outsz) {
    wchar_t wide_buf[4096];
    DWORD n = GetModuleFileNameW(NULL, wide_buf, (DWORD)(sizeof(wide_buf) / sizeof(wide_buf[0])));
    if (n > 0 && n < (sizeof(wide_buf) / sizeof(wide_buf[0]))) {
        char *utf8 = cbm_wide_to_utf8(wide_buf);
        if (!utf8) {
            return false;
        }
        bool copied = copy_path(out, outsz, utf8);
        free(utf8);
        return copied;
    }
    return false;
}
#endif

static bool root_path_for_project(const char *project, char *root_path, size_t root_path_size) {
    if (!root_path || root_path_size == 0) {
        return false;
    }
    root_path[0] = '\0';
    char db_path[1024];
    db_path_for_project(project, db_path, sizeof(db_path));
    if (db_path[0] == '\0' || !cbm_file_exists(db_path)) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        return false;
    }
    cbm_project_t stored_project;
    memset(&stored_project, 0, sizeof(stored_project));
    bool found = cbm_store_get_project(store, project, &stored_project) == CBM_STORE_OK &&
                 stored_project.root_path && stored_project.root_path[0];
    if (found) {
        snprintf(root_path, root_path_size, "%s", stored_project.root_path);
    }
    cbm_project_free_fields(&stored_project);
    cbm_store_close(store);
    return found;
}

bool cbm_http_server_resolve_binary_path(const char *argv0, char *out, size_t outsz) {
    if (!out || outsz == 0) {
        return false;
    }
    out[0] = '\0';

#ifndef _WIN32
    if (argv0 && strchr(argv0, '/') && is_executable_file(argv0)) {
        return copy_path(out, outsz, argv0);
    }
    if (resolve_from_path(argv0, out, outsz)) {
        return true;
    }
#else
    if (argv0 && argv0[0]) {
        wchar_t *wide_argv0 = cbm_utf8_to_wide(argv0);
        DWORD attrs = wide_argv0 ? GetFileAttributesW(wide_argv0) : INVALID_FILE_ATTRIBUTES;
        free(wide_argv0);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return copy_path(out, outsz, argv0);
        }
    }
#endif

    if (resolve_self_executable(out, outsz)) {
        return true;
    }
    return copy_path(out, outsz, argv0);
}

void cbm_http_server_set_binary_path(const char *path) {
    if (path) {
        if (!cbm_http_server_resolve_binary_path(path, g_binary_path, sizeof(g_binary_path))) {
            g_binary_path[0] = '\0';
        }
    }
}

/* Index via subprocess — isolates crashes from the main process. */
static void set_job_error(index_job_t *job, const char *message) {
    if (!job) {
        return;
    }
    char sanitized[256];
    snprintf(sanitized, sizeof(sanitized), "%s",
             message && message[0] ? message : "operation failed");
    for (char *p = sanitized; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    snprintf(job->error_msg, sizeof(job->error_msg), "%s", sanitized);
    cbm_mutex_unlock(&g_index_jobs_mutex);
}

static bool job_error_empty(index_job_t *job) {
    if (!job) {
        return true;
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    bool empty = job->error_msg[0] == '\0';
    cbm_mutex_unlock(&g_index_jobs_mutex);
    return empty;
}

static void copy_job_start_fields(int slot, unsigned long long *job_id, char *root_path,
                                  size_t root_path_size) {
    if (job_id) {
        *job_id = 0;
    }
    if (root_path && root_path_size > 0) {
        root_path[0] = '\0';
    }
    if (slot < 0 || slot >= MAX_INDEX_JOBS) {
        return;
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    if (job_id) {
        *job_id = g_index_jobs[slot].job_id;
    }
    if (root_path && root_path_size > 0) {
        snprintf(root_path, root_path_size, "%s", g_index_jobs[slot].root_path);
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
}

/* Index workers return an MCP result through --response-out and deliberately
 * exit with zero once that response is written. Keep tool-level failures
 * distinguishable from process failures for the UI job status. */
static bool index_response_has_error(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size <= 0 || size > 8L * 1024L * 1024L || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *data = malloc((size_t)size + 1U);
    if (!data) {
        fclose(file);
        return false;
    }
    size_t read_count = fread(data, 1U, (size_t)size, file);
    fclose(file);
    data[read_count] = '\0';
    yyjson_doc *doc = read_count == (size_t)size ? yyjson_read(data, read_count, 0) : NULL;
    free(data);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool is_error = root && yyjson_is_true(yyjson_obj_get(root, "isError"));
    yyjson_doc_free(doc);
    return is_error;
}

static void ui_index_log_line(const char *line, void *ud) {
    (void)ud;
    if (line && line[0]) {
        cbm_ui_log_append(line);
    }
}

static void ui_index_child_pid(long pid, void *ud) {
    index_job_t *job = ud;
    if (!job || pid <= 0) {
        return;
    }
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    if (atomic_load(&job->status) == 1) {
        job->child_pid = pid;
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
}

static int ui_index_quiet_timeout_ms(void) {
    enum { DEFAULT_TIMEOUT_MS = 900000 }; /* 15 minutes without a log line */
    const char *value = getenv("CBM_INDEX_WORKER_TIMEOUT_S");
    if (!value || !value[0]) {
        return DEFAULT_TIMEOUT_MS;
    }
    char *end = NULL;
    long seconds = strtol(value, &end, 10);
    if (end == value || *end != '\0' || seconds <= 0 || seconds > INT_MAX / 1000) {
        return DEFAULT_TIMEOUT_MS;
    }
    return (int)(seconds * 1000L);
}

static void *index_thread_fn(void *arg) {
    index_job_t *job = arg;
    index_job_snapshot_t snapshot = {0};
    snapshot_index_job(job, &snapshot);
    char root_path[sizeof(snapshot.root_path)];
    snprintf(root_path, sizeof(root_path), "%s", snapshot.root_path);
    cbm_log_info("ui.index.start", "path", root_path);

    if (snapshot.remote_url[0]) {
        char remote_error[1024] = {0};
        if (cbm_remote_repo_prepare(snapshot.project_name, snapshot.remote_url,
                                    snapshot.remote_branch, snapshot.poll_interval_sec, root_path,
                                    sizeof(root_path), remote_error, sizeof(remote_error)) != 0) {
            set_job_error(job, remote_error);
            set_job_status(job, 3);
            cbm_log_warn("ui.remote.prepare_failed", "project", snapshot.project_name, "detail",
                         remote_error);
            return NULL;
        }
        index_jobs_init();
        cbm_mutex_lock(&g_index_jobs_mutex);
        snprintf(job->root_path, sizeof(job->root_path), "%s", root_path);
        cbm_mutex_unlock(&g_index_jobs_mutex);
        cbm_log_info("ui.remote.prepared", "project", snapshot.project_name, "branch",
                     snapshot.remote_branch);
    }

    /* Use stored binary path, or try to find it */
    const char *bin = g_binary_path;
    char self_path[1024] = {0};
    if (!bin[0]) {
        cbm_http_server_resolve_binary_path(NULL, self_path, sizeof(self_path));
        bin = self_path[0] ? self_path : "codebase-memory-mcp";
    }

    char log_file[1024];
    char response_file[1024];

    /* JSON-escape root_path and optional project name. */
    char escaped_path[2048];
    cbm_json_escape(escaped_path, (int)sizeof(escaped_path), root_path);
    char escaped_name[512];
    cbm_json_escape(escaped_name, (int)sizeof(escaped_name), snapshot.project_name);
    char json_arg[4096];
    if (snapshot.project_name[0]) {
        snprintf(json_arg, sizeof(json_arg), "{\"repo_path\":\"%s\",\"name\":\"%s\"}", escaped_path,
                 escaped_name);
    } else {
        snprintf(json_arg, sizeof(json_arg), "{\"repo_path\":\"%s\"}", escaped_path);
    }

#ifdef _WIN32
    wchar_t wide_temp[768];
    wchar_t wide_log[1024];
    wchar_t wide_response[1024];
    DWORD temp_len = GetTempPathW((DWORD)(sizeof(wide_temp) / sizeof(wide_temp[0])), wide_temp);
    if (temp_len > 0 && temp_len < (sizeof(wide_temp) / sizeof(wide_temp[0]))) {
        int written =
            swprintf(wide_log, sizeof(wide_log) / sizeof(wide_log[0]), L"%lscbm_index_%d_%llu.log",
                     wide_temp, (int)_getpid(), snapshot.job_id);
        int response_written =
            swprintf(wide_response, sizeof(wide_response) / sizeof(wide_response[0]),
                     L"%lscbm_index_%d_%llu.response", wide_temp, (int)_getpid(), snapshot.job_id);
        char *utf8_log = written > 0 ? cbm_wide_to_utf8(wide_log) : NULL;
        char *utf8_response = response_written > 0 ? cbm_wide_to_utf8(wide_response) : NULL;
        if (!utf8_log || !copy_path(log_file, sizeof(log_file), utf8_log)) {
            snprintf(log_file, sizeof(log_file), "cbm_index_%d_%llu.log", (int)_getpid(),
                     snapshot.job_id);
        }
        if (!utf8_response || !copy_path(response_file, sizeof(response_file), utf8_response)) {
            snprintf(response_file, sizeof(response_file), "cbm_index_%d_%llu.response",
                     (int)_getpid(), snapshot.job_id);
        }
        free(utf8_log);
        free(utf8_response);
    } else {
        snprintf(log_file, sizeof(log_file), "cbm_index_%d_%llu.log", (int)_getpid(),
                 snapshot.job_id);
        snprintf(response_file, sizeof(response_file), "cbm_index_%d_%llu.response", (int)_getpid(),
                 snapshot.job_id);
    }

#else
    snprintf(log_file, sizeof(log_file), "/tmp/cbm_index_%d_%llu.log", (int)getpid(),
             snapshot.job_id);
    snprintf(response_file, sizeof(response_file), "/tmp/cbm_index_%d_%llu.response", (int)getpid(),
             snapshot.job_id);
#endif

    /* The shared subprocess runner handles argv quoting, safe POSIX fork/exec,
     * log tailing, crash classification, and a quiet timeout on every platform.
     * The UI worker is explicitly marked so it runs the index in-process once;
     * the runner itself is the isolation boundary. */
    const char *const idx_argv[] = {
        bin,           "cli", "--index-worker", "index_repository", json_arg, "--response-out",
        response_file, NULL};
    cbm_proc_opts_t proc_opts = {0};
    proc_opts.bin = bin;
    proc_opts.argv = idx_argv;
    proc_opts.log_file = log_file;
    proc_opts.on_log_line = ui_index_log_line;
    proc_opts.on_child_pid = ui_index_child_pid;
    proc_opts.child_pid_ud = job;
    proc_opts.quiet_timeout_ms = ui_index_quiet_timeout_ms();
    proc_opts.delete_log_on_exit = false;

    cbm_log_info("ui.index.spawn", "bin", bin, "log", log_file);
    cbm_proc_result_t proc_result = {0};
    int spawn_rc = cbm_subprocess_run(&proc_opts, &proc_result);
    int exit_code = spawn_rc != 0 ? -1 : proc_result.exit_code;
    if (spawn_rc == 0 && proc_result.outcome != CBM_PROC_CLEAN) {
        char error[256];
        snprintf(error, sizeof(error), "indexing worker ended with %s (exit code %d)",
                 cbm_proc_outcome_str(proc_result.outcome), proc_result.exit_code);
        set_job_error(job, error);
        exit_code = -1;
    } else if (exit_code == 0 && index_response_has_error(response_file)) {
        exit_code = 1;
    }
    (void)cbm_unlink(log_file);
    (void)cbm_unlink(response_file);

    if (exit_code != 0) {
        if (job_error_empty(job)) {
            char error[128];
            snprintf(error, sizeof(error), "indexing failed (exit code %d)", exit_code);
            set_job_error(job, error);
        }
        set_job_status(job, 3);
    } else {
        set_job_status(job, 2);
        if (snapshot.remote_url[0] && snapshot.watcher) {
            cbm_watcher_watch(snapshot.watcher, snapshot.project_name, root_path);
        }
    }
    cbm_log_info("ui.index.done", "path", root_path, "rc", exit_code == 0 ? "ok" : "err");
    return NULL;
}

enum {
    INDEX_JOB_NO_SLOT = -1,
    INDEX_JOB_THREAD_FAILED = -2,
};

static int start_index_job(cbm_http_server_t *srv, const char *root_path, const char *project_name,
                           const char *remote_url, const char *remote_branch,
                           int poll_interval_sec) {
    /* The UI MCP instance caches the last queried SQLite store. On Windows that
     * open handle prevents the index worker's atomic temp-DB replacement, so a
     * fully successful parse otherwise ends as phase=dump / exit code 1. Drop
     * named-project stores before every local or remote background update. The
     * initial in-memory store is intentionally preserved by evict_idle(). */
    cbm_mcp_server_evict_idle(srv->mcp, 0);

    index_jobs_init();
    for (;;) {
        int slot = INDEX_JOB_NO_SLOT;
        int reap_slot = INDEX_JOB_NO_SLOT;
        cbm_thread_t reap_thread;
        unsigned long long reap_job_id = 0;

        cbm_mutex_lock(&g_index_jobs_mutex);
        for (int i = 0; i < MAX_INDEX_JOBS; i++) {
            int status = atomic_load(&g_index_jobs[i].status);
            bool same_named_project = project_name && project_name[0] &&
                                      strcmp(g_index_jobs[i].project_name, project_name) == 0;
            bool same_unnamed_root = (!project_name || !project_name[0]) &&
                                     g_index_jobs[i].project_name[0] == '\0' && root_path &&
                                     strcmp(g_index_jobs[i].root_path, root_path) == 0;
            if (status == 1 && (same_named_project || same_unnamed_root)) {
                cbm_mutex_unlock(&g_index_jobs_mutex);
                /* Starting the same operation twice is idempotent. */
                return i;
            }
            bool completed_reaped = (status == 2 || status == 3) && !g_index_jobs[i].thread_valid &&
                                    !g_index_jobs[i].reaping;
            if (slot == INDEX_JOB_NO_SLOT && (status == 0 || completed_reaped)) {
                slot = i;
            }
            if (reap_slot == INDEX_JOB_NO_SLOT && (status == 2 || status == 3) &&
                g_index_jobs[i].thread_valid && !g_index_jobs[i].reaping) {
                reap_slot = i;
                g_index_jobs[i].reaping = true;
                reap_thread = g_index_jobs[i].thread;
                reap_job_id = g_index_jobs[i].job_id;
            }
        }
        if (slot == INDEX_JOB_NO_SLOT && reap_slot == INDEX_JOB_NO_SLOT) {
            cbm_mutex_unlock(&g_index_jobs_mutex);
            return INDEX_JOB_NO_SLOT;
        }
        if (slot == INDEX_JOB_NO_SLOT) {
            cbm_mutex_unlock(&g_index_jobs_mutex);
            (void)cbm_thread_join(&reap_thread);
            cbm_mutex_lock(&g_index_jobs_mutex);
            index_job_t *reaped = &g_index_jobs[reap_slot];
            if (reaped->job_id == reap_job_id && reaped->reaping) {
                reaped->thread_valid = false;
                reaped->reaping = false;
                atomic_store(&reaped->status, 0);
                reaped->error_msg[0] = '\0';
            }
            cbm_mutex_unlock(&g_index_jobs_mutex);
            continue;
        }

        index_job_t *job = &g_index_jobs[slot];
        job->job_id = atomic_fetch_add(&g_next_index_job_id, 1);
        snprintf(job->root_path, sizeof(job->root_path), "%s", root_path);
        snprintf(job->project_name, sizeof(job->project_name), "%s",
                 project_name ? project_name : "");
        snprintf(job->remote_url, sizeof(job->remote_url), "%s", remote_url ? remote_url : "");
        snprintf(job->remote_branch, sizeof(job->remote_branch), "%s",
                 remote_branch ? remote_branch : "");
        job->poll_interval_sec = poll_interval_sec;
        job->watcher = srv->watcher;
        job->error_msg[0] = '\0';
        job->thread_valid = false;
        job->reaping = false;
        job->child_pid = 0;
        atomic_store(&job->status, 1);

        /* Hold the mutex through creation so the worker cannot finish and make
         * this slot appear reusable before its native handle is recorded. */
        if (cbm_thread_create(&job->thread, 0, index_thread_fn, job) != 0) {
            atomic_store(&job->status, 3);
            snprintf(job->error_msg, sizeof(job->error_msg), "%s", "thread creation failed");
            cbm_mutex_unlock(&g_index_jobs_mutex);
            return INDEX_JOB_THREAD_FAILED;
        }
        job->thread_valid = true;
        cbm_mutex_unlock(&g_index_jobs_mutex);
        return slot;
    }
}

static bool reply_index_job_error(cbm_http_conn_t *c, int result) {
    if (result == INDEX_JOB_NO_SLOT) {
        cbm_http_replyf(c, 429, g_cors_json, "{\"error\":\"all index slots busy\"}");
        return true;
    }
    if (result == INDEX_JOB_THREAD_FAILED) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"thread creation failed\"}");
        return true;
    }
    return false;
}

/* POST /api/index — body: {"root_path": "/abs/path", "project_name": "..."} */
static void handle_index_start(cbm_http_server_t *srv, cbm_http_conn_t *c,
                               const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 4096) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v_path = yyjson_obj_get(root, "root_path");
    if (!v_path || !yyjson_is_str(v_path)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing root_path\"}");
        return;
    }
    const char *rpath = yyjson_get_str(v_path);
    yyjson_val *v_project_name = yyjson_obj_get(root, "project_name");
    const char *project_name = yyjson_is_str(v_project_name) ? yyjson_get_str(v_project_name) : "";

    /* Check path exists */
    if (!cbm_is_dir(rpath)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"directory not found\"}");
        return;
    }

    char canonical_root[CBM_SZ_4K];
    char boundary_err[CBM_SZ_1K];
    if (!cbm_canonical_path(rpath, canonical_root, sizeof(canonical_root))) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"cannot resolve root_path\"}");
        return;
    }
    cbm_normalize_path_sep(canonical_root);
    if (!cbm_workspace_root_allowed(canonical_root, cbm_workspace_home_dir(),
                                    cbm_workspace_cache_dir(), getenv("CBM_ALLOWED_ROOT"),
                                    boundary_err, sizeof(boundary_err))) {
        yyjson_doc_free(doc);
        char escaped[CBM_SZ_2K];
        cbm_json_escape(escaped, (int)sizeof(escaped), boundary_err);
        cbm_http_replyf(c, 403, g_cors_json, "{\"error\":\"%s\"}", escaped);
        return;
    }

    int slot = start_index_job(srv, canonical_root, project_name, NULL, NULL, 0);
    yyjson_doc_free(doc);
    if (reply_index_job_error(c, slot)) {
        return;
    }

    unsigned long long job_id = 0;
    char job_path[1024];
    copy_job_start_fields(slot, &job_id, job_path, sizeof(job_path));
    char escaped_path[2048];
    cbm_json_escape(escaped_path, (int)sizeof(escaped_path), job_path);
    cbm_http_replyf(c, 202, g_cors_json,
                    "{\"status\":\"indexing\",\"job_id\":%llu,\"slot\":%d,\"path\":\"%s\"}", job_id,
                    slot, escaped_path);
}

/* POST /api/remote-index — clone a managed SSH repository and index it. */
static void handle_remote_index_start(cbm_http_server_t *srv, cbm_http_conn_t *c,
                                      const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 4096) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }
    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *url_value = yyjson_obj_get(root, "remote_url");
    yyjson_val *branch_value = yyjson_obj_get(root, "branch");
    yyjson_val *project_value = yyjson_obj_get(root, "project_name");
    yyjson_val *poll_value = yyjson_obj_get(root, "poll_interval_sec");
    const char *remote_url = yyjson_is_str(url_value) ? yyjson_get_str(url_value) : NULL;
    const char *branch = yyjson_is_str(branch_value) ? yyjson_get_str(branch_value) : "main";
    const char *requested_project =
        yyjson_is_str(project_value) ? yyjson_get_str(project_value) : "";
    int poll_interval_sec = yyjson_is_int(poll_value) ? (int)yyjson_get_int(poll_value) : 300;

    char normalized_url[CBM_REMOTE_URL_MAX] = {0};
    if (!cbm_remote_repo_normalize_url(remote_url, normalized_url, sizeof(normalized_url))) {
        yyjson_doc_free(doc);
        cbm_http_replyf(
            c, 400, g_cors_json,
            "{\"error\":\"unsupported repository URL; use an SSH or HTTPS repository URL\"}");
        return;
    }
    if (!cbm_remote_repo_validate_branch(branch)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid branch name\"}");
        return;
    }
    if (poll_interval_sec < 60 || poll_interval_sec > 3600) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json,
                        "{\"error\":\"poll interval must be between 60 and 3600 seconds\"}");
        return;
    }

    char project_name[CBM_REMOTE_PROJECT_MAX] = {0};
    bool project_ok = false;
    if (requested_project[0]) {
        size_t project_len = strlen(requested_project);
        project_ok =
            project_len < sizeof(project_name) && cbm_validate_project_name(requested_project);
        if (project_ok) {
            memcpy(project_name, requested_project, project_len + 1);
        }
    } else {
        project_ok =
            cbm_remote_repo_default_project(normalized_url, project_name, sizeof(project_name));
    }
    if (!project_ok) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid project ID\"}");
        return;
    }

    char managed_path[1024];
    if (!cbm_remote_repo_managed_path(project_name, managed_path, sizeof(managed_path))) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot resolve managed path\"}");
        return;
    }
    char current_root[1024];
    if (root_path_for_project(project_name, current_root, sizeof(current_root)) &&
        strcmp(current_root, managed_path) != 0) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 409, g_cors_json,
                        "{\"error\":\"project ID is already used by another repository\"}");
        return;
    }

    int slot =
        start_index_job(srv, managed_path, project_name, normalized_url, branch, poll_interval_sec);
    yyjson_doc_free(doc);
    if (reply_index_job_error(c, slot)) {
        return;
    }

    char escaped_path[2048];
    char escaped_project[512];
    unsigned long long job_id = 0;
    char job_path[1024];
    copy_job_start_fields(slot, &job_id, job_path, sizeof(job_path));
    cbm_json_escape(escaped_path, (int)sizeof(escaped_path), job_path);
    cbm_json_escape(escaped_project, (int)sizeof(escaped_project), project_name);
    cbm_http_replyf(c, 202, g_cors_json,
                    "{\"status\":\"indexing\",\"job_id\":%llu,\"slot\":%d,"
                    "\"path\":\"%s\",\"project\":\"%s\"}",
                    job_id, slot, escaped_path, escaped_project);
}

static void handle_remote_info(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char project[256] = {0};
    if (!cbm_http_query_param(req->query, "project", project, (int)sizeof(project)) ||
        !cbm_validate_project_name(project)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project\"}");
        return;
    }
    char root_path[1024];
    if (!root_path_for_project(project, root_path, sizeof(root_path))) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }
    cbm_remote_repo_config_t config;
    if (cbm_remote_repo_load(root_path, &config) != 0) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"managed\":false}");
        return;
    }
    char escaped_url[2048];
    char escaped_branch[512];
    cbm_json_escape(escaped_url, (int)sizeof(escaped_url), config.remote_url);
    cbm_json_escape(escaped_branch, (int)sizeof(escaped_branch), config.branch);
    cbm_http_replyf(
        c, 200, g_cors_json,
        "{\"managed\":true,\"remote_url\":\"%s\",\"branch\":\"%s\",\"poll_interval_sec\":%d}",
        escaped_url, escaped_branch, config.poll_interval_sec);
}

/* POST /api/project-update?name=X — refresh source, then rebuild its graph. */
static void handle_project_update(cbm_http_server_t *srv, cbm_http_conn_t *c,
                                  const cbm_http_req_t *req) {
    char project[256] = {0};
    if (!cbm_http_query_param(req->query, "name", project, (int)sizeof(project)) ||
        !cbm_validate_project_name(project)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project\"}");
        return;
    }

    char root_path[1024];
    if (!root_path_for_project(project, root_path, sizeof(root_path))) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }
    if (!cbm_is_dir(root_path)) {
        cbm_http_replyf(c, 409, g_cors_json, "{\"error\":\"repository directory is unavailable\"}");
        return;
    }

    cbm_remote_repo_config_t config;
    bool is_remote = cbm_remote_repo_load(root_path, &config) == 0;
    int slot =
        start_index_job(srv, root_path, project, is_remote ? config.remote_url : NULL,
                        is_remote ? config.branch : NULL, is_remote ? config.poll_interval_sec : 0);
    if (reply_index_job_error(c, slot)) {
        return;
    }

    char escaped_path[2048];
    char escaped_project[512];
    cbm_json_escape(escaped_path, (int)sizeof(escaped_path), root_path);
    cbm_json_escape(escaped_project, (int)sizeof(escaped_project), project);
    unsigned long long job_id = 0;
    copy_job_start_fields(slot, &job_id, NULL, 0);
    cbm_http_replyf(c, 202, g_cors_json,
                    "{\"status\":\"indexing\",\"job_id\":%llu,\"slot\":%d,\"path\":\"%s\","
                    "\"project\":\"%s\",\"source\":\"%s\"}",
                    job_id, slot, escaped_path, escaped_project, is_remote ? "remote" : "local");
}

static void handle_remote_sync(cbm_http_server_t *srv, cbm_http_conn_t *c,
                               const cbm_http_req_t *req) {
    char project[256] = {0};
    if (!cbm_http_query_param(req->query, "project", project, (int)sizeof(project)) ||
        !cbm_validate_project_name(project)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project\"}");
        return;
    }
    char root_path[1024];
    cbm_remote_repo_config_t config;
    if (!root_path_for_project(project, root_path, sizeof(root_path)) ||
        cbm_remote_repo_load(root_path, &config) != 0) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"managed repository not found\"}");
        return;
    }
    if (!srv->watcher) {
        cbm_http_replyf(c, 503, g_cors_json, "{\"error\":\"watcher unavailable\"}");
        return;
    }
    cbm_watcher_touch(srv->watcher, project);
    cbm_http_replyf(c, 202, g_cors_json, "{\"status\":\"scheduled\"}");
}

/* GET /api/index-status — returns status of all index jobs */
static void handle_index_status(cbm_http_conn_t *c) {
    char buf[16384] = "[";
    int pos = 1;
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        int st = atomic_load(&g_index_jobs[i].status);
        if (st == 0)
            continue;
        if (pos > 1)
            http_appendf(buf, sizeof(buf), &pos, ",");
        const char *ss = st == 1 ? "indexing" : st == 2 ? "done" : "error";
        char escaped_path[2048];
        char escaped_project[512];
        char escaped_error[512];
        cbm_json_escape(escaped_path, (int)sizeof(escaped_path), g_index_jobs[i].root_path);
        cbm_json_escape(escaped_project, (int)sizeof(escaped_project),
                        g_index_jobs[i].project_name);
        cbm_json_escape(escaped_error, (int)sizeof(escaped_error),
                        st == 3 ? g_index_jobs[i].error_msg : "");
        http_appendf(buf, sizeof(buf), &pos,
                     "{\"job_id\":%llu,\"slot\":%d,\"status\":\"%s\",\"path\":\"%s\","
                     "\"project\":\"%s\",\"source\":\"%s\",\"error\":\"%s\"}",
                     g_index_jobs[i].job_id, i, ss, escaped_path, escaped_project,
                     g_index_jobs[i].remote_url[0] ? "remote" : "local", escaped_error);
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
    http_appendf(buf, sizeof(buf), &pos, "]");
    if ((size_t)pos >= sizeof(buf)) {
        pos = (int)sizeof(buf) - 1;
    }
    buf[pos] = '\0';
    cbm_http_replyf(c, 200, g_cors_json, "%s", buf);
}

/* W5 admin API: expose the same project directory used by MCP list_projects.
 * The MCP handler returns a text content envelope; unwrap it so admin clients
 * receive a stable JSON object directly. This endpoint is intentionally served
 * by the loopback UI server, whose host guard already limits administrative
 * access to the local machine. */
static void handle_admin_projects(cbm_http_server_t *srv, cbm_http_conn_t *c) {
    char *result = cbm_mcp_handle_tool(srv->mcp, "list_projects", "{\"metadata_only\":true}");
    if (!result) {
        cbm_http_replyf(c, 503, g_cors_json, "{\"error\":\"project directory unavailable\"}");
        return;
    }
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *content = root ? yyjson_obj_get(root, "content") : NULL;
    yyjson_val *first = content && yyjson_is_arr(content) ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = first ? yyjson_obj_get(first, "text") : NULL;
    const char *payload = text && yyjson_is_str(text) ? yyjson_get_str(text) : NULL;
    if (payload) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", payload);
    } else {
        cbm_http_replyf(c, 503, g_cors_json, "{\"error\":\"invalid project directory response\"}");
    }
    if (doc)
        yyjson_doc_free(doc);
    free(result);
}

/* Liveness/readiness probes for desktop and remote management wrappers. */
static void handle_healthz(cbm_http_conn_t *c) {
    cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"ok\"}");
}

static void handle_readyz(cbm_http_server_t *srv, cbm_http_conn_t *c) {
    if (srv && srv->listener_ok) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"ready\"}");
    } else {
        cbm_http_replyf(c, 503, g_cors_json, "{\"status\":\"starting\"}");
    }
}

/* Small, dependency-free Prometheus surface. Counters are derived from the
 * bounded in-memory job history; this keeps metrics useful after restarts while
 * avoiding a new persistence format for the MVP. */
static void handle_metrics(cbm_http_conn_t *c) {
    int active = 0, done = 0, failed = 0, seen = 0;
    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        int st = atomic_load(&g_index_jobs[i].status);
        if (st == 1)
            active++;
        else if (st == 2)
            done++;
        else if (st == 3)
            failed++;
        if (st != 0)
            seen++;
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
    char body[1024];
    int n = snprintf(body, sizeof(body),
                     "# TYPE cbm_index_jobs_active gauge\n"
                     "cbm_index_jobs_active %d\n"
                     "# TYPE cbm_index_jobs_completed gauge\n"
                     "cbm_index_jobs_completed %d\n"
                     "# TYPE cbm_index_jobs_failed gauge\n"
                     "cbm_index_jobs_failed %d\n"
                     "# TYPE cbm_index_jobs_known gauge\n"
                     "cbm_index_jobs_known %d\n",
                     active, done, failed, seen);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cbm_http_replyf(c, 500, g_cors, "metrics unavailable");
        return;
    }
    cbm_http_replyf(c, 200,
                    "Content-Type: text/plain; version=0.0.4\r\nCache-Control: no-store\r\n", "%s",
                    body);
}

/* DELETE /api/project?name=X — closes the UI store, then deletes the project. */
static void handle_delete_project(cbm_http_server_t *srv, cbm_http_conn_t *c,
                                  const cbm_http_req_t *req) {
    char name[256] = {0};
    if (!cbm_http_query_param(req->query, "name", name, (int)sizeof(name)) || name[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing name\"}");
        return;
    }
    if (!cbm_validate_project_name(name)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid project name\"}");
        return;
    }

    index_jobs_init();
    cbm_mutex_lock(&g_index_jobs_mutex);
    bool project_busy = false;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load(&g_index_jobs[i].status) == 1 &&
            strcmp(g_index_jobs[i].project_name, name) == 0) {
            project_busy = true;
            break;
        }
    }
    cbm_mutex_unlock(&g_index_jobs_mutex);
    if (project_busy) {
        cbm_http_replyf(c, 409, g_cors_json, "{\"error\":\"project is currently being indexed\"}");
        return;
    }

    char escaped_name[512];
    cbm_json_escape(escaped_name, (int)sizeof(escaped_name), name);
    char args[640];
    int args_len = snprintf(args, sizeof(args), "{\"project\":\"%s\"}", escaped_name);
    if (args_len <= 0 || (size_t)args_len >= sizeof(args)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid project name\"}");
        return;
    }

    char managed_root[1024] = {0};
    bool has_managed_root = root_path_for_project(name, managed_root, sizeof(managed_root));

    /* The MCP implementation closes its cached SQLite store before unlinking.
     * Calling it here matters on Windows, where an open database handle prevents
     * deletion; the previous direct unlink always failed after the UI loaded a graph. */
    char *tool_result = cbm_mcp_handle_tool(srv->mcp, "delete_project", args);
    if (!tool_result) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"delete failed\"}");
        return;
    }

    yyjson_doc *result_doc = yyjson_read(tool_result, strlen(tool_result), 0);
    yyjson_val *result_root = result_doc ? yyjson_doc_get_root(result_doc) : NULL;
    yyjson_val *content = result_root ? yyjson_obj_get(result_root, "content") : NULL;
    yyjson_val *first = content && yyjson_is_arr(content) ? yyjson_arr_get(content, 0U) : NULL;
    yyjson_val *text_val = first ? yyjson_obj_get(first, "text") : NULL;
    const char *payload = text_val && yyjson_is_str(text_val) ? yyjson_get_str(text_val) : NULL;
    if (!payload) {
        if (result_doc)
            yyjson_doc_free(result_doc);
        free(tool_result);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"invalid delete response\"}");
        return;
    }

    int http_status = yyjson_is_true(yyjson_obj_get(result_root, "isError")) ? 409 : 200;
    yyjson_doc *payload_doc = yyjson_read(payload, strlen(payload), 0);
    yyjson_val *payload_root = payload_doc ? yyjson_doc_get_root(payload_doc) : NULL;
    yyjson_val *status_val = payload_root ? yyjson_obj_get(payload_root, "status") : NULL;
    const char *status =
        status_val && yyjson_is_str(status_val) ? yyjson_get_str(status_val) : NULL;
    if (status && strcmp(status, "not_found") == 0) {
        /* DELETE is idempotent: the requested end state already exists. */
        http_status = 200;
    }

    if (http_status == 200) {
        if (has_managed_root) {
            int cleanup = cbm_remote_repo_remove_managed(name, managed_root);
            if (cleanup < 0) {
                cbm_log_warn("ui.project.clone_cleanup_failed", "name", name, "path", managed_root);
            }
        }
        cbm_log_info("ui.project.deleted", "name", name);
    } else {
        cbm_log_warn("ui.project.delete_failed", "name", name, "detail", payload);
    }
    cbm_http_replyf(c, http_status, g_cors_json, "%s", payload);
    if (payload_doc)
        yyjson_doc_free(payload_doc);
    yyjson_doc_free(result_doc);
    free(tool_result);
}

/* GET /api/project-health?name=X — checks db integrity */
static void handle_project_health(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char name[256] = {0};
    if (!cbm_http_query_param(req->query, "name", name, (int)sizeof(name)) || name[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing name\"}");
        return;
    }
    if (index_job_running_for_project(name)) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"indexing\"}");
        return;
    }

    char db_path[1024];
    db_path_for_project(name, db_path, sizeof(db_path));

    if (!cbm_file_exists(db_path)) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"missing\"}");
        return;
    }

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"corrupt\",\"reason\":\"cannot open\"}");
        return;
    }

    int node_count = cbm_store_count_nodes(store, name);
    int edge_count = cbm_store_count_edges(store, name);
    cbm_store_close(store);

    int64_t size = cbm_file_size(db_path);

    cbm_http_replyf(c, 200, g_cors_json,
                    "{\"status\":\"healthy\",\"nodes\":%d,\"edges\":%d,\"size_bytes\":%lld}",
                    node_count, edge_count, (long long)size);
}

/* ── Handle GET /api/layout ───────────────────────────────────── */

/* Find distinct target_project values from CROSS_* edges in a store.
 * Writes up to max_out project names (heap-allocated). Returns count. */
static int find_cross_repo_targets(cbm_store_t *store, const char *project, char **out,
                                   int max_out) {
    struct sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return 0;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT DISTINCT json_extract(properties, '$.target_project') FROM edges "
            "WHERE project = ?1 AND type LIKE 'CROSS_%' "
            "AND json_extract(properties, '$.target_project') IS NOT NULL",
            -1, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, 1, project, -1, SQLITE_STATIC);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < max_out) {
        const char *tp = (const char *)sqlite3_column_text(s, 0);
        if (tp && tp[0]) {
            size_t len = strlen(tp);
            out[count] = malloc(len + 1);
            memcpy(out[count], tp, len + 1);
            count++;
        }
    }
    sqlite3_finalize(s);
    return count;
}

enum { LAYOUT_MAX_LINKED = 16 };
#define LAYOUT_GALAXY_SPACING 600.0
#define LAYOUT_GALAXY_PAD 400.0

typedef struct {
    int64_t source;
    int64_t target;
    char *type;
} layout_cross_edge_t;

typedef struct {
    layout_cross_edge_t *edges;
    int count;
    int capacity;
    int64_t *target_ids;
    int target_count;
    int target_capacity;
} layout_cross_set_t;

static bool layout_id_list_append_unique(int64_t **ids, int *count, int *capacity, int64_t id) {
    if (id <= 0)
        return false;
    for (int i = 0; i < *count; i++) {
        if ((*ids)[i] == id)
            return true;
    }
    if (*count >= *capacity) {
        int next = *capacity > 0 ? *capacity * 2 : 64;
        int64_t *grown = realloc(*ids, (size_t)next * sizeof(*grown));
        if (!grown)
            return false;
        *ids = grown;
        *capacity = next;
    }
    (*ids)[(*count)++] = id;
    return true;
}

static bool layout_cross_set_append(layout_cross_set_t *set, int64_t source, int64_t target,
                                    const char *type) {
    if (!set || source <= 0 || target <= 0 || !type)
        return false;
    if (set->count >= set->capacity) {
        int next = set->capacity > 0 ? set->capacity * 2 : 64;
        layout_cross_edge_t *grown = realloc(set->edges, (size_t)next * sizeof(*grown));
        if (!grown)
            return false;
        set->edges = grown;
        set->capacity = next;
    }
    char *type_copy = strdup(type);
    if (!type_copy)
        return false;
    set->edges[set->count++] =
        (layout_cross_edge_t){.source = source, .target = target, .type = type_copy};
    (void)layout_id_list_append_unique(&set->target_ids, &set->target_count, &set->target_capacity,
                                       target);
    return true;
}

static void layout_cross_set_free(layout_cross_set_t *set) {
    if (!set)
        return;
    for (int i = 0; i < set->count; i++)
        free(set->edges[i].type);
    free(set->edges);
    free(set->target_ids);
    memset(set, 0, sizeof(*set));
}

static bool layout_has_node(const cbm_layout_result_t *layout, int64_t id) {
    if (!layout)
        return false;
    for (int i = 0; i < layout->node_count; i++) {
        if (layout->nodes[i].id == id)
            return true;
    }
    return false;
}

/* Pin caller nodes before the primary layout is sampled. The set is unique
 * and ordered by node id, matching the bounded, deterministic pinning policy
 * in layout3d.c. */
static int64_t *find_cross_source_ids(cbm_store_t *store, const char *project, int *out_count) {
    *out_count = 0;
    struct sqlite3 *db = cbm_store_get_db(store);
    if (!db)
        return NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT DISTINCT source_id FROM edges "
                           "WHERE project = ?1 AND type LIKE 'CROSS_%' "
                           "ORDER BY source_id",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    int64_t *ids = NULL;
    int count = 0, capacity = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!layout_id_list_append_unique(&ids, &count, &capacity, sqlite3_column_int64(stmt, 0)))
            break;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return ids;
}

/* Resolve CROSS_* records against the linked store. Identity metadata wins;
 * the local Route QN is only a compatibility fallback for old indexes. */
static void find_resolved_cross_edges(cbm_store_t *source_store, const char *source_project,
                                      cbm_store_t *target_store, const char *target_project,
                                      layout_cross_set_t *out) {
    struct sqlite3 *db = cbm_store_get_db(source_store);
    if (!db || !target_store || !out)
        return;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT e.source_id, e.type, n.qualified_name, "
                      "json_extract(e.properties, '$.target_qualified_name'), "
                      "json_extract(e.properties, '$.target_file'), "
                      "json_extract(e.properties, '$.target_function') "
                      "FROM edges e LEFT JOIN nodes n "
                      "  ON n.id = e.target_id AND n.project = e.project "
                      "WHERE e.project = ?1 AND e.type LIKE 'CROSS_%' "
                      "  AND json_valid(e.properties) "
                      "  AND json_extract(e.properties, '$.target_project') = ?2 "
                      "ORDER BY e.source_id, e.id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, source_project, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, target_project, -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t source = sqlite3_column_int64(stmt, 0);
        const char *type = (const char *)sqlite3_column_text(stmt, 1);
        const char *route_qn = (const char *)sqlite3_column_text(stmt, 2);
        const char *target_qn = (const char *)sqlite3_column_text(stmt, 3);
        const char *target_file = (const char *)sqlite3_column_text(stmt, 4);
        const char *target_function = (const char *)sqlite3_column_text(stmt, 5);
        int64_t target = cbm_layout_resolve_cross_target(target_store, target_project, target_qn,
                                                         target_file, target_function, route_qn);
        if (target > 0 && type)
            (void)layout_cross_set_append(out, source, target, type);
    }
    sqlite3_finalize(stmt);
}

/* Bounding-radius of a layout result: max distance from origin across all
 * nodes. Used to size galaxy spacing so satellites don't overlap the primary
 * cluster. Layouts with a 1000-node cluster have radius ~1500; the previous
 * fixed 600 spacing buried satellites inside the primary mass. */
static double layout_radius(const cbm_layout_result_t *r) {
    if (!r || r->node_count == 0)
        return 0.0;
    double max_r2 = 0.0;
    for (int i = 0; i < r->node_count; i++) {
        double x = (double)r->nodes[i].x;
        double y = (double)r->nodes[i].y;
        double z = (double)r->nodes[i].z;
        if (!isfinite(x) || !isfinite(y) || !isfinite(z))
            continue;
        double r2 = x * x + y * y + z * z;
        if (r2 > max_r2)
            max_r2 = r2;
    }
    return sqrt(max_r2);
}

/* Attach the missed-graph skeleton (#963) to the primary layout doc as
 *   "missed_graph": {"nodes":[...], "edges":[...], "offset":{x,y,z}}
 * — the file structure of files the indexer could not fully cover, laid out
 * as a satellite cluster beside the code galaxy (the UI renders it as a white
 * skeleton; clicking it re-centers the camera there). The offset sits on the
 * -Y side: linked-project satellites spread counter-clockwise from +X, so
 * this slot collides last. Returns true when a non-empty skeleton was
 * attached; no-op when the project has no missed files. */
static bool attach_missed_graph(yyjson_mut_doc *mdoc, yyjson_mut_val *mroot, cbm_store_t *store,
                                const char *project, double primary_radius) {
    char covproj[512];
    cbm_store_coverage_shadow_project(covproj, sizeof(covproj), project);
    cbm_layout_result_t *ml = cbm_layout_compute(store, covproj, CBM_LAYOUT_OVERVIEW, NULL, 0, 0);
    if (!ml) {
        return false;
    }
    if (ml->node_count == 0) {
        cbm_layout_free(ml);
        return false;
    }
    double miss_radius = layout_radius(ml);
    char *mjson = cbm_layout_to_json(ml);
    cbm_layout_free(ml);
    if (!mjson) {
        return false;
    }
    yyjson_doc *mldoc = yyjson_read(mjson, strlen(mjson), 0);
    free(mjson);
    if (!mldoc) {
        return false;
    }
    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_val *mlroot = yyjson_doc_get_root(mldoc);
    yyjson_val *mn = yyjson_obj_get(mlroot, "nodes");
    yyjson_val *me = yyjson_obj_get(mlroot, "edges");
    if (mn) {
        yyjson_mut_obj_add_val(mdoc, entry, "nodes", yyjson_val_mut_copy(mdoc, mn));
    }
    if (me) {
        yyjson_mut_obj_add_val(mdoc, entry, "edges", yyjson_val_mut_copy(mdoc, me));
    }
    yyjson_doc_free(mldoc);

    double dist = primary_radius + miss_radius + LAYOUT_GALAXY_PAD;
    if (dist < LAYOUT_GALAXY_SPACING) {
        dist = LAYOUT_GALAXY_SPACING;
    }
    yyjson_mut_val *offset = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_real(mdoc, offset, "x", 0.0);
    yyjson_mut_obj_add_real(mdoc, offset, "y", -dist);
    yyjson_mut_obj_add_real(mdoc, offset, "z", 0.0);
    yyjson_mut_obj_add_val(mdoc, entry, "offset", offset);

    yyjson_mut_obj_add_val(mdoc, mroot, "missed_graph", entry);
    return true;
}

static void handle_layout(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char project[256] = {0};
    char max_str[32] = {0};
    char graph_str[32] = {0};

    if (!cbm_http_query_param(req->query, "project", project, (int)sizeof(project)) ||
        project[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project parameter\"}");
        return;
    }
    if (index_job_running_for_project(project)) {
        cbm_http_replyf(c, 409, g_cors_json,
                        "{\"error\":\"this project graph is being updated; retry after indexing "
                        "completes\"}");
        return;
    }

    int max_nodes = 0; /* 0 → layout default budget */
    if (cbm_http_query_param(req->query, "max_nodes", max_str, (int)sizeof(max_str))) {
        int v = atoi(max_str);
        if (v > 0)
            max_nodes = v;
    }

    /* graph=missed (#963): lay out the derived miss graph (shadow project
     * "<name>::missed" inside the SAME db file) instead of the code graph —
     * only files the indexer could not fully cover, as their file structure.
     * The db file still resolves from the validated base project name. */
    bool missed_graph = false;
    if (cbm_http_query_param(req->query, "graph", graph_str, (int)sizeof(graph_str))) {
        missed_graph = strcmp(graph_str, "missed") == 0;
    }
    char scoped_project[320];
    if (missed_graph) {
        cbm_store_coverage_shadow_project(scoped_project, sizeof(scoped_project), project);
    } else {
        snprintf(scoped_project, sizeof(scoped_project), "%s", project);
    }

    char db_path[1024];
    db_path_for_project(project, db_path, sizeof(db_path));

    if (!cbm_file_exists(db_path)) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot open store\"}");
        return;
    }

    /* Discover cross-project callers before sampling the primary graph so the
     * finite render budget keeps link endpoints visible. */
    char *linked[LAYOUT_MAX_LINKED];
    int linked_count = find_cross_repo_targets(store, project, linked, LAYOUT_MAX_LINKED);
    int required_source_count = 0;
    int64_t *required_source_ids = find_cross_source_ids(store, project, &required_source_count);
    cbm_layout_result_t *layout = cbm_layout_compute_with_required_nodes(
        store, scoped_project, CBM_LAYOUT_OVERVIEW, NULL, 0, max_nodes, required_source_ids,
        required_source_count);
    free(required_source_ids);

    if (!layout) {
        for (int i = 0; i < linked_count; i++)
            free(linked[i]);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"layout computation failed\"}");
        return;
    }

    /* Capture primary cluster radius before freeing the layout. */
    double primary_radius = layout_radius(layout);

    /* Build JSON: primary layout + linked_projects */
    char *primary_json = cbm_layout_to_json(layout);
    if (!primary_json) {
        cbm_layout_free(layout);
        for (int i = 0; i < linked_count; i++)
            free(linked[i]);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON serialization failed\"}");
        return;
    }

    /* Fast path: no satellites to attach. The missed skeleton only decorates
     * the CODE graph — a graph=missed request already IS the miss graph. */
    if (linked_count == 0 && missed_graph) {
        cbm_layout_free(layout);
        cbm_store_close(store);
        cbm_http_replyf(c, 200, g_cors_json, "%s", primary_json);
        free(primary_json);
        return;
    }

    /* Parse primary JSON and append missed_graph + linked_projects */
    yyjson_doc *pdoc = yyjson_read(primary_json, strlen(primary_json), 0);
    free(primary_json);
    if (!pdoc) {
        cbm_layout_free(layout);
        for (int i = 0; i < linked_count; i++)
            free(linked[i]);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON parse failed\"}");
        return;
    }

    yyjson_mut_doc *mdoc = yyjson_doc_mut_copy(pdoc, NULL);
    yyjson_doc_free(pdoc);
    yyjson_mut_val *mroot = yyjson_mut_doc_get_root(mdoc);

    if (!missed_graph) {
        (void)attach_missed_graph(mdoc, mroot, store, project, primary_radius);
    }

    yyjson_mut_val *lp_arr = yyjson_mut_arr(mdoc);

    for (int li = 0; li < linked_count; li++) {
        if (index_job_running_for_project(linked[li])) {
            free(linked[li]);
            continue;
        }
        char lp_path[1024];
        db_path_for_project(linked[li], lp_path, sizeof(lp_path));
        if (!cbm_file_exists(lp_path)) {
            free(linked[li]);
            continue;
        }

        cbm_store_t *lp_store = cbm_store_open_path(lp_path);
        if (!lp_store) {
            free(linked[li]);
            continue;
        }

        layout_cross_set_t cross = {0};
        find_resolved_cross_edges(store, project, lp_store, linked[li], &cross);
        cbm_layout_result_t *lp_layout = cbm_layout_compute_with_required_nodes(
            lp_store, linked[li], CBM_LAYOUT_OVERVIEW, NULL, 0, max_nodes, cross.target_ids,
            cross.target_count);

        if (!lp_layout) {
            layout_cross_set_free(&cross);
            cbm_store_close(lp_store);
            free(linked[li]);
            continue;
        }

        double sat_radius = layout_radius(lp_layout);
        char *lp_json = cbm_layout_to_json(lp_layout);
        if (!lp_json) {
            cbm_layout_free(lp_layout);
            layout_cross_set_free(&cross);
            cbm_store_close(lp_store);
            free(linked[li]);
            continue;
        }

        /* Parse linked project layout */
        yyjson_doc *lpdoc = yyjson_read(lp_json, strlen(lp_json), 0);
        free(lp_json);
        if (!lpdoc) {
            cbm_layout_free(lp_layout);
            layout_cross_set_free(&cross);
            cbm_store_close(lp_store);
            free(linked[li]);
            continue;
        }

        yyjson_mut_doc *lm = yyjson_doc_mut_copy(lpdoc, NULL);
        yyjson_doc_free(lpdoc);
        yyjson_mut_val *lmroot = yyjson_mut_doc_get_root(lm);

        /* Build linked project entry */
        yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_strcpy(mdoc, entry, "project", linked[li]);

        /* Copy nodes and edges from linked layout */
        yyjson_mut_val *ln = yyjson_mut_obj_get(lmroot, "nodes");
        yyjson_mut_val *le = yyjson_mut_obj_get(lmroot, "edges");
        if (ln) {
            yyjson_mut_obj_add_val(mdoc, entry, "nodes", yyjson_mut_val_mut_copy(mdoc, ln));
        }
        if (le) {
            yyjson_mut_obj_add_val(mdoc, entry, "edges", yyjson_mut_val_mut_copy(mdoc, le));
        }

        /* Compute galaxy offset: evenly spaced around primary, far enough out
         * that the primary cluster (radius primary_radius) and the satellite
         * cluster (radius sat_radius) don't overlap. Bounded below by
         * LAYOUT_GALAXY_SPACING for trivially small projects. */
        double angle = (2.0 * 3.14159265358979) * (double)li / (double)linked_count;
        double dist = primary_radius + sat_radius + LAYOUT_GALAXY_PAD;
        if (dist < LAYOUT_GALAXY_SPACING) {
            dist = LAYOUT_GALAXY_SPACING;
        }
        yyjson_mut_val *offset = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_real(mdoc, offset, "x", cos(angle) * dist);
        yyjson_mut_obj_add_real(mdoc, offset, "y", sin(angle) * dist);
        yyjson_mut_obj_add_real(mdoc, offset, "z", 0.0);
        yyjson_mut_obj_add_val(mdoc, entry, "offset", offset);

        /* Only emit cross edges whose pinned endpoints survived the configured
         * node budget. This keeps every returned edge directly renderable. */
        yyjson_mut_val *cross_arr = yyjson_mut_arr(mdoc);
        for (int ci = 0; ci < cross.count; ci++) {
            layout_cross_edge_t *edge = &cross.edges[ci];
            if (!layout_has_node(layout, edge->source) || !layout_has_node(lp_layout, edge->target))
                continue;
            yyjson_mut_val *ce = yyjson_mut_obj(mdoc);
            yyjson_mut_obj_add_int(mdoc, ce, "source", edge->source);
            yyjson_mut_obj_add_int(mdoc, ce, "target", edge->target);
            yyjson_mut_obj_add_strcpy(mdoc, ce, "type", edge->type);
            yyjson_mut_arr_append(cross_arr, ce);
        }
        yyjson_mut_obj_add_val(mdoc, entry, "cross_edges", cross_arr);

        cbm_layout_free(lp_layout);
        layout_cross_set_free(&cross);
        cbm_store_close(lp_store);
        yyjson_mut_arr_append(lp_arr, entry);
        yyjson_mut_doc_free(lm);
        free(linked[li]);
    }

    cbm_layout_free(layout);
    cbm_store_close(store);
    yyjson_mut_obj_add_val(mdoc, mroot, "linked_projects", lp_arr);

    size_t len = 0;
    char *final_json = yyjson_mut_write(mdoc, 0, &len);
    yyjson_mut_doc_free(mdoc);

    if (final_json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", final_json);
        free(final_json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON write failed\"}");
    }
}

/* ── Handle JSON-RPC request ──────────────────────────────────── */

static void handle_rpc(cbm_http_conn_t *c, const cbm_http_req_t *req, cbm_mcp_server_t *mcp) {
    if (req->body_len == 0 || req->body_len > MAX_BODY_SIZE || !req->body) {
        cbm_http_replyf(c, 400, g_cors_json,
                        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
                        "\"message\":\"invalid request size\"},\"id\":null}");
        return;
    }

    /* req->body is NUL-terminated by the transport */
    char *response = cbm_mcp_server_handle(mcp, req->body);

    if (response) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", response);
        free(response);
    } else {
        cbm_http_replyf(c, 204, g_cors, "%s", "");
    }
}

/* ── Request dispatch ─────────────────────────────────────────── */

/* True when the Host header names the loopback interface the server binds to
 * (with or without a port). Anything else means the request reached us under a
 * name that is not loopback — a rebinding DNS host or a proxy pointed at the
 * local port — which is the DNS-rebinding / cross-site vector against a
 * localhost-only service. */
static bool host_is_loopback(const char *host) {
    return cbm_http_path_match(host, "localhost") || cbm_http_path_match(host, "localhost:*") ||
           cbm_http_path_match(host, "127.0.0.1") || cbm_http_path_match(host, "127.0.0.1:*") ||
           cbm_http_path_match(host, "[::1]") || cbm_http_path_match(host, "[::1]:*");
}

static void dispatch_request(cbm_http_server_t *srv, cbm_http_conn_t *c,
                             const cbm_http_req_t *req) {
    /* Build per-request CORS headers (only reflects localhost origins) */
    update_cors(req);

    /* DNS-rebinding / cross-site guard: the server binds to loopback only, so a
     * request carrying any non-loopback Host was routed here under a foreign
     * name (a rebinding DNS record, a proxy) and must be refused before it can
     * reach a state-changing endpoint. A bare request with no Host header
     * (HTTP/1.0 local tooling) is still allowed. */
    if (req->host[0] != '\0' && !host_is_loopback(req->host)) {
        cbm_http_replyf(c, 403, g_cors, "%s", "{\"error\":\"forbidden host\"}");
        return;
    }

    bool is_get = strcmp(req->method, "GET") == 0;
    bool is_post = strcmp(req->method, "POST") == 0;
    bool is_delete = strcmp(req->method, "DELETE") == 0;

    /* OPTIONS preflight for CORS */
    if (strcmp(req->method, "OPTIONS") == 0) {
        cbm_http_replyf(c, 204, g_cors, "%s", "");
        return;
    }

    /* W5 local admin API. These aliases intentionally reuse the existing
     * project/index handlers so deduplication and worker isolation stay in one
     * place. */
    if (is_get && cbm_http_path_match(req->path, "/healthz")) {
        handle_healthz(c);
        return;
    }
    if (is_get && cbm_http_path_match(req->path, "/readyz")) {
        handle_readyz(srv, c);
        return;
    }
    if (is_get && cbm_http_path_match(req->path, "/metrics")) {
        handle_metrics(c);
        return;
    }
    if (is_get && cbm_http_path_match(req->path, "/admin/v1/projects")) {
        handle_admin_projects(srv, c);
        return;
    }
    if (is_get && cbm_http_path_match(req->path, "/admin/v1/jobs")) {
        handle_index_status(c);
        return;
    }
    if (is_post && cbm_http_path_match(req->path, "/admin/v1/index")) {
        handle_index_start(srv, c, req);
        return;
    }
    if (is_post && cbm_http_path_match(req->path, "/admin/v1/projects")) {
        handle_index_start(srv, c, req);
        return;
    }
    if (is_post && cbm_http_path_match(req->path, "/admin/v1/remote-index")) {
        handle_remote_index_start(srv, c, req);
        return;
    }
    if (is_post && cbm_http_path_match(req->path, "/admin/v1/projects/update*")) {
        handle_project_update(srv, c, req);
        return;
    }
    if (is_delete && cbm_http_path_match(req->path, "/admin/v1/projects*")) {
        handle_delete_project(srv, c, req);
        return;
    }

    /* POST /rpc → JSON-RPC dispatch (reuses existing MCP tools) */
    if (is_post && cbm_http_path_match(req->path, "/rpc")) {
        /* Do not reopen the database currently being replaced. Independent
         * projects remain readable, so one update cannot blank every graph. */
        if (rpc_targets_running_project(req->body, req->body_len)) {
            cbm_http_replyf(c, 409, g_cors_json,
                            "{\"error\":\"requested graph update in progress; retry after indexing "
                            "completes\"}");
            return;
        }
        handle_rpc(c, req, srv->mcp);
        return;
    }

    /* GET /api/layout → 3D graph layout */
    if (is_get && cbm_http_path_match(req->path, "/api/layout*")) {
        handle_layout(c, req);
        return;
    }

    /* GET /api/repo-info → git remote / branch for GitHub deep-links */
    if (is_get && cbm_http_path_match(req->path, "/api/repo-info*")) {
        handle_repo_info(c, req);
        return;
    }

    /* POST /api/index → start background indexing */
    if (is_post && cbm_http_path_match(req->path, "/api/index")) {
        handle_index_start(srv, c, req);
        return;
    }

    /* POST /api/project-update → refresh local/Git source and rebuild its graph */
    if (is_post && cbm_http_path_match(req->path, "/api/project-update*")) {
        handle_project_update(srv, c, req);
        return;
    }

    /* POST /api/remote-index → clone a managed Git repository and index it */
    if (is_post && cbm_http_path_match(req->path, "/api/remote-index")) {
        handle_remote_index_start(srv, c, req);
        return;
    }

    /* GET /api/remote-info → managed remote settings for a project */
    if (is_get && cbm_http_path_match(req->path, "/api/remote-info*")) {
        handle_remote_info(c, req);
        return;
    }

    /* POST /api/remote-sync → schedule an immediate remote poll */
    if (is_post && cbm_http_path_match(req->path, "/api/remote-sync*")) {
        handle_remote_sync(srv, c, req);
        return;
    }

    /* GET /api/index-status → check indexing progress */
    if (is_get && cbm_http_path_match(req->path, "/api/index-status")) {
        handle_index_status(c);
        return;
    }

    /* GET /api/ui-config → language and local UI preferences */
    if (is_get && cbm_http_path_match(req->path, "/api/ui-config")) {
        handle_ui_config(c, req);
        return;
    }

    /* Client integration preview and explicitly-confirmed reconciliation. */
    if (is_get && cbm_http_path_match(req->path, "/api/integrations")) {
        handle_integrations_get(c);
        return;
    }
    if (is_post && cbm_http_path_match(req->path, "/api/integrations/apply")) {
        handle_integrations_apply(c, req);
        return;
    }

    /* DELETE /api/project → delete a project's .db file */
    if (is_delete && cbm_http_path_match(req->path, "/api/project*")) {
        handle_delete_project(srv, c, req);
        return;
    }

    /* GET /api/browse → directory browser for file picker */
    if (is_get && cbm_http_path_match(req->path, "/api/browse*")) {
        handle_browse(c, req);
        return;
    }

    /* GET /api/adr → get ADR for project */
    if (is_get && cbm_http_path_match(req->path, "/api/adr*")) {
        handle_adr_get(c, req);
        return;
    }

    /* POST /api/adr → save ADR for project */
    if (is_post && cbm_http_path_match(req->path, "/api/adr")) {
        handle_adr_save(c, req);
        return;
    }

    /* GET /api/project-health → check db integrity */
    if (is_get && cbm_http_path_match(req->path, "/api/project-health*")) {
        handle_project_health(c, req);
        return;
    }

    /* GET /api/processes → list running codebase-memory-mcp processes */
    if (is_get && cbm_http_path_match(req->path, "/api/processes")) {
        handle_processes(c);
        return;
    }

    /* GET /api/logs → recent log lines */
    if (is_get && cbm_http_path_match(req->path, "/api/logs*")) {
        handle_logs(c, req);
        return;
    }

    /* GET /api/usage-stats → persistent AI MCP call statistics */
    if (is_get && cbm_http_path_match(req->path, "/api/usage-stats*")) {
        handle_usage_stats(c, req);
        return;
    }

    /* POST /api/process-kill → kill a process */
    if (is_post && cbm_http_path_match(req->path, "/api/process-kill")) {
        handle_process_kill(c, req);
        return;
    }

    /* GET / → index.html (no-cache so browser always gets latest) */
    if (cbm_http_path_match(req->path, "/")) {
        const cbm_embedded_file_t *f = cbm_embedded_lookup("/index.html");
        if (f) {
            char html_hdrs[1024];
            snprintf(html_hdrs, sizeof(html_hdrs),
                     "%sContent-Type: text/html\r\nCache-Control: no-cache\r\n" CBM_UI_CSP, g_cors);
            cbm_http_reply_buf(c, 200, html_hdrs, f->data, (size_t)f->size);
            return;
        }
        cbm_http_replyf(c, 404, g_cors, "no frontend embedded");
        return;
    }

    /* GET /assets/... → embedded assets, then generic embedded fallback */
    if (serve_embedded(c, req->path))
        return;

    cbm_http_replyf(c, 404, g_cors, "not found");
}

/* ── Public API ───────────────────────────────────────────────── */

cbm_http_server_t *cbm_http_server_new(int port) {
    cbm_http_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->port = port;
    atomic_store(&srv->stop_flag, 0);

    /* Create a dedicated MCP server for HTTP (own SQLite connection) */
    srv->mcp = cbm_mcp_server_new(NULL);
    if (!srv->mcp) {
        cbm_log_error("ui.http.mcp_fail", "reason", "cannot create MCP instance");
        free(srv);
        return NULL;
    }

    /* Bind to localhost only (httpd refuses anything else by construction) */
    srv->listener = cbm_httpd_listen(port);
    if (!srv->listener) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        cbm_log_warn("ui.unavailable", "port", port_str, "reason", "in_use", "hint",
                     "use --port=N to override");
        cbm_mcp_server_free(srv->mcp);
        free(srv);
        return NULL;
    }

    srv->port = cbm_httpd_port(srv->listener);
    srv->listener_ok = true;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", srv->port);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", srv->port);
    cbm_log_info("ui.serving", "url", url, "port", port_str);

    return srv;
}

static void join_all_index_jobs(void) {
    index_jobs_init();
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        index_job_t *job = &g_index_jobs[i];
        cbm_thread_t thread;
        unsigned long long job_id = 0;
        bool should_join = false;
        cbm_mutex_lock(&g_index_jobs_mutex);
        if (job->thread_valid && !job->reaping) {
            job->reaping = true;
            thread = job->thread;
            job_id = job->job_id;
            should_join = true;
        }
        cbm_mutex_unlock(&g_index_jobs_mutex);
        if (!should_join) {
            continue;
        }
        (void)cbm_thread_join(&thread);
        cbm_mutex_lock(&g_index_jobs_mutex);
        if (job->job_id == job_id && job->reaping) {
            job->thread_valid = false;
            job->reaping = false;
            atomic_store(&job->status, 0);
        }
        cbm_mutex_unlock(&g_index_jobs_mutex);
    }
}

void cbm_http_server_free(cbm_http_server_t *srv) {
    if (!srv)
        return;
    join_all_index_jobs();
    cbm_httpd_close(srv->listener);
    cbm_mcp_server_free(srv->mcp);
    free(srv);
}

void cbm_http_server_stop(cbm_http_server_t *srv) {
    if (srv) {
        atomic_store(&srv->stop_flag, 1);
    }
}

void cbm_http_server_run(cbm_http_server_t *srv) {
    if (!srv || !srv->listener_ok)
        return;

    while (!atomic_load(&srv->stop_flag)) {
        cbm_http_conn_t *conn = cbm_httpd_accept(srv->listener, 200);
        if (!conn)
            continue; /* timeout — re-check stop flag */

        uint64_t request_start_ms = cbm_now_ms();
        cbm_http_req_t req;
        int rc = cbm_httpd_read_request(conn, &req);
        if (rc == 0) {
            dispatch_request(srv, conn, &req);
            cbm_log_http_request("graph_ui", req.method, req.path, cbm_http_conn_status(conn),
                                 (int64_t)(cbm_now_ms() - request_start_ms), req.body_len,
                                 cbm_http_conn_response_bytes(conn));
            cbm_http_req_free(&req);
        } else if (rc > 0) {
            /* Parse/transport error with a known HTTP status (400/408/411/413/431).
             * No CORS reflection here — the request was never parsed. */
            cbm_http_replyf(conn, rc, "", "bad request");
            cbm_log_http_request("graph_ui", "", "", cbm_http_conn_status(conn),
                                 (int64_t)(cbm_now_ms() - request_start_ms), 0,
                                 cbm_http_conn_response_bytes(conn));
        }
        cbm_httpd_conn_close(conn);
    }
}

bool cbm_http_server_is_running(const cbm_http_server_t *srv) {
    return srv && srv->listener_ok;
}

int cbm_http_server_port(const cbm_http_server_t *srv) {
    return (srv && srv->listener_ok) ? srv->port : -1;
}

void cbm_http_server_set_recv_deadline_ms(cbm_http_server_t *srv, int ms) {
    if (srv && srv->listener_ok) {
        cbm_httpd_set_recv_deadline_ms(srv->listener, ms);
    }
}

void cbm_http_server_set_watcher(cbm_http_server_t *srv, struct cbm_watcher *watcher) {
    if (srv) {
        srv->watcher = watcher;
        cbm_mcp_server_set_watcher(srv->mcp, watcher);
    }
}
