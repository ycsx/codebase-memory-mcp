#include "mcp/usage_stats.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "store/store.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USAGE_DB_NAME "_usage.db"
#define USAGE_RETENTION_DAYS 30
#define USAGE_MAX_FILES_PER_CALL 16
#define USAGE_FILE_LIMIT_DEFAULT 12
#define USAGE_FILE_LIMIT_MAX 100

struct cbm_usage_store {
    sqlite3 *db;
};

typedef struct {
    char values[USAGE_MAX_FILES_PER_CALL][CBM_SZ_1K];
    int count;
} usage_files_t;

static char *usage_strdup(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t len = strlen(value);
    char *copy = malloc(len + 1U);
    if (copy) {
        memcpy(copy, value, len + 1U);
    }
    return copy;
}

static bool usage_db_path(char *out, size_t out_size) {
    const char *cache = cbm_resolve_cache_dir();
    if (!out || out_size == 0 || !cache || cache[0] == '\0') {
        return false;
    }
    if (!cbm_mkdir_p(cache, 0750)) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/%s", cache, USAGE_DB_NAME);
    return written > 0 && (size_t)written < out_size;
}

static bool usage_schema(sqlite3 *db) {
    static const char schema[] =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS usage_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts INTEGER NOT NULL,"
        " tool TEXT NOT NULL,"
        " project TEXT NOT NULL DEFAULT '',"
        " is_error INTEGER NOT NULL DEFAULT 0,"
        " duration_us INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS usage_event_files ("
        " event_id INTEGER NOT NULL REFERENCES usage_events(id) ON DELETE CASCADE,"
        " project TEXT NOT NULL DEFAULT '',"
        " file_path TEXT NOT NULL,"
        " PRIMARY KEY(event_id, project, file_path)"
        ");"
        "CREATE INDEX IF NOT EXISTS usage_events_ts_idx ON usage_events(ts);"
        "CREATE INDEX IF NOT EXISTS usage_files_path_idx "
        " ON usage_event_files(project, file_path, event_id);";
    return sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK;
}

cbm_usage_store_t *cbm_usage_store_open(void) {
    char path[CBM_SZ_2K];
    if (!usage_db_path(path, sizeof(path))) {
        return NULL;
    }

    cbm_usage_store_t *store = calloc(1U, sizeof(*store));
    if (!store) {
        return NULL;
    }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &store->db, flags, NULL) != SQLITE_OK) {
        cbm_usage_store_close(store);
        return NULL;
    }
    sqlite3_busy_timeout(store->db, 150);
    if (!usage_schema(store->db)) {
        cbm_usage_store_close(store);
        return NULL;
    }

    return store;
}

void cbm_usage_store_close(cbm_usage_store_t *store) {
    if (!store) {
        return;
    }
    if (store->db) {
        sqlite3_close(store->db);
    }
    free(store);
}

static bool has_pattern_meta(const char *value) {
    return value && strpbrk(value, "*?[]{}()|+^$\\") != NULL;
}

static bool looks_like_file_path(const char *value) {
    if (!value || value[0] == '\0') {
        return false;
    }
    if (strchr(value, '/') || strchr(value, '\\')) {
        return true;
    }
    const char *dot = strrchr(value, '.');
    if (!dot || dot == value || dot[1] == '\0') {
        return false;
    }
    size_t ext_len = strlen(dot + 1);
    if (ext_len > 12U) {
        return false;
    }
    for (const char *p = dot + 1; *p; p++) {
        if (!isalnum((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void usage_add_file(usage_files_t *files, const char *value, bool reject_patterns) {
    if (!files || !value || files->count >= USAGE_MAX_FILES_PER_CALL) {
        return;
    }
    while (isspace((unsigned char)*value)) {
        value++;
    }
    if (strncmp(value, "file:", 5U) == 0) {
        value += 5;
    }
    while (value[0] == '.' && (value[1] == '/' || value[1] == '\\')) {
        value += 2;
    }
    if (value[0] == '\0' || (reject_patterns && has_pattern_meta(value))) {
        return;
    }

    char normalized[CBM_SZ_1K];
    size_t length = strlen(value);
    while (length > 0 && isspace((unsigned char)value[length - 1])) {
        length--;
    }
    if (length == 0 || length >= sizeof(normalized)) {
        return;
    }
    for (size_t i = 0; i < length; i++) {
        normalized[i] = value[i] == '\\' ? '/' : value[i];
    }
    normalized[length] = '\0';

    for (int i = 0; i < files->count; i++) {
        if (strcmp(files->values[i], normalized) == 0) {
            return;
        }
    }
    snprintf(files->values[files->count++], CBM_SZ_1K, "%s", normalized);
}

static const char *json_string(yyjson_val *root, const char *key) {
    yyjson_val *value = root && yyjson_is_obj(root) ? yyjson_obj_get(root, key) : NULL;
    return value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static bool usage_resolve_symbol(cbm_store_t *graph_store, const char *project, const char *symbol,
                                 usage_files_t *files) {
    if (!graph_store || !project || !symbol || symbol[0] == '\0') {
        return false;
    }

    cbm_node_t node = {0};
    if (cbm_store_find_node_by_qn(graph_store, project, symbol, &node) == CBM_STORE_OK) {
        if (node.file_path && node.file_path[0]) {
            usage_add_file(files, node.file_path, false);
        }
        bool found = node.file_path && node.file_path[0];
        cbm_node_free_fields(&node);
        return found;
    }

    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_qn_suffix(graph_store, project, symbol, &nodes, &count) ==
            CBM_STORE_OK &&
        count == 1 && nodes[0].file_path && nodes[0].file_path[0]) {
        usage_add_file(files, nodes[0].file_path, false);
        cbm_store_free_nodes(nodes, count);
        return true;
    }
    cbm_store_free_nodes(nodes, count);

    nodes = NULL;
    count = 0;
    if (cbm_store_find_nodes_by_name(graph_store, project, symbol, &nodes, &count) ==
            CBM_STORE_OK &&
        count == 1 && nodes[0].file_path && nodes[0].file_path[0]) {
        usage_add_file(files, nodes[0].file_path, false);
        cbm_store_free_nodes(nodes, count);
        return true;
    }
    cbm_store_free_nodes(nodes, count);
    return false;
}

static void usage_collect_files(const char *tool_name, const char *args_json,
                                cbm_store_t *graph_store, const char *current_project,
                                char project[CBM_SZ_256], usage_files_t *files) {
    if (!args_json || !project || !files) {
        return;
    }
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }

    const char *project_value = json_string(root, "project");
    if (project_value) {
        snprintf(project, CBM_SZ_256, "%s", project_value);
    }
    bool store_matches =
        graph_store && project[0] && current_project && strcmp(project, current_project) == 0;

    usage_add_file(files, json_string(root, "file_path"), false);
    const char *file_pattern = json_string(root, "file_pattern");
    if (file_pattern && looks_like_file_path(file_pattern)) {
        usage_add_file(files, file_pattern, true);
    }

    yyjson_val *paths = yyjson_obj_get(root, "paths");
    if (paths && yyjson_is_arr(paths)) {
        size_t index, max;
        yyjson_val *path;
        yyjson_arr_foreach(paths, index, max, path) {
            if (yyjson_is_str(path)) {
                usage_add_file(files, yyjson_get_str(path), false);
            }
        }
    }

    const char *qualified_name = json_string(root, "qualified_name");
    const char *function_name = json_string(root, "function_name");
    if (store_matches && qualified_name) {
        (void)usage_resolve_symbol(graph_store, project, qualified_name, files);
    }
    if (store_matches && function_name) {
        (void)usage_resolve_symbol(graph_store, project, function_name, files);
    }

    const char *path = json_string(root, "path");
    if (path && (!store_matches || !usage_resolve_symbol(graph_store, project, path, files)) &&
        looks_like_file_path(path)) {
        usage_add_file(files, path, true);
    }

    if (tool_name && strcmp(tool_name, "explain_impact") == 0) {
        const char *target = json_string(root, "target");
        const char *query = json_string(root, "query");
        const char *candidate = target && target[0] ? target : query;
        if (candidate && strncmp(candidate, "file:", 5U) == 0) {
            usage_add_file(files, candidate, false);
        } else if (candidate && store_matches &&
                   usage_resolve_symbol(graph_store, project, candidate, files)) {
            /* Symbol target resolved to its defining file. */
        } else if (candidate && looks_like_file_path(candidate)) {
            usage_add_file(files, candidate, true);
        }
    }

    yyjson_doc_free(doc);
}

void cbm_usage_record(cbm_usage_store_t *store, const char *tool_name, const char *args_json,
                      cbm_store_t *graph_store, const char *current_project, bool is_error,
                      int64_t duration_us) {
    if (!store || !store->db || !tool_name || tool_name[0] == '\0') {
        return;
    }

    char project[CBM_SZ_256] = "";
    usage_files_t files = {0};
    usage_collect_files(tool_name, args_json ? args_json : "{}", graph_store, current_project,
                        project, &files);

    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        return;
    }

    sqlite3_stmt *event = NULL;
    int rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO usage_events(ts,tool,project,is_error,duration_us) VALUES(?1,?2,?3,?4,?5)", -1,
        &event, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(event, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(event, 2, tool_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(event, 3, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(event, 4, is_error ? 1 : 0);
        sqlite3_bind_int64(event, 5, duration_us);
        rc = sqlite3_step(event);
    }
    sqlite3_finalize(event);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    sqlite3_int64 event_id = sqlite3_last_insert_rowid(store->db);
    sqlite3_stmt *file = NULL;
    rc = sqlite3_prepare_v2(
        store->db,
        "INSERT OR IGNORE INTO usage_event_files(event_id,project,file_path) VALUES(?1,?2,?3)", -1,
        &file, NULL);
    if (rc == SQLITE_OK) {
        for (int i = 0; i < files.count; i++) {
            sqlite3_reset(file);
            sqlite3_clear_bindings(file);
            sqlite3_bind_int64(file, 1, event_id);
            sqlite3_bind_text(file, 2, project, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(file, 3, files.values[i], -1, SQLITE_TRANSIENT);
            if (sqlite3_step(file) != SQLITE_DONE) {
                rc = SQLITE_ERROR;
                break;
            }
        }
    }
    sqlite3_finalize(file);
    if ((rc == SQLITE_OK || rc == SQLITE_DONE) && event_id % 256 == 0) {
        sqlite3_stmt *cleanup = NULL;
        if (sqlite3_prepare_v2(store->db, "DELETE FROM usage_events WHERE ts < ?1", -1, &cleanup,
                               NULL) == SQLITE_OK) {
            time_t cutoff = time(NULL) - (time_t)USAGE_RETENTION_DAYS * 24 * 60 * 60;
            sqlite3_bind_int64(cleanup, 1, (sqlite3_int64)cutoff);
            (void)sqlite3_step(cleanup);
        }
        sqlite3_finalize(cleanup);
    }
    sqlite3_exec(store->db, rc == SQLITE_OK || rc == SQLITE_DONE ? "COMMIT" : "ROLLBACK", NULL,
                 NULL, NULL);
}

static char *usage_empty_json(bool available) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "{\"available\":%s,\"retention_days\":%d,\"total_calls\":0,"
             "\"calls_last_minute\":0,\"calls_last_hour\":0,\"errors\":0,"
             "\"active_files\":0,\"unattributed_calls\":0,\"last_called_at_ms\":0,"
             "\"files\":[]}",
             available ? "true" : "false", USAGE_RETENTION_DAYS);
    return usage_strdup(buffer);
}

char *cbm_usage_stats_json(int file_limit) {
    if (file_limit <= 0) {
        file_limit = USAGE_FILE_LIMIT_DEFAULT;
    } else if (file_limit > USAGE_FILE_LIMIT_MAX) {
        file_limit = USAGE_FILE_LIMIT_MAX;
    }

    cbm_usage_store_t *store = cbm_usage_store_open();
    if (!store) {
        return usage_empty_json(false);
    }

    time_t now = time(NULL);
    int64_t total = 0;
    int64_t last_minute = 0;
    int64_t last_hour = 0;
    int64_t errors = 0;
    int64_t unattributed = 0;
    int64_t last_called = 0;
    int64_t active_files = 0;

    sqlite3_stmt *summary = NULL;
    static const char summary_sql[] =
        "SELECT COUNT(*),COALESCE(SUM(ts>=?1),0),COALESCE(SUM(ts>=?2),0),"
        "COALESCE(SUM(is_error),0),COALESCE(MAX(ts),0),"
        "COALESCE(SUM(NOT EXISTS(SELECT 1 FROM usage_event_files f WHERE f.event_id=e.id)),0) "
        "FROM usage_events e WHERE ts>=?3";
    if (sqlite3_prepare_v2(store->db, summary_sql, -1, &summary, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(summary, 1, (sqlite3_int64)(now - 60));
        sqlite3_bind_int64(summary, 2, (sqlite3_int64)(now - 3600));
        sqlite3_bind_int64(summary, 3,
                           (sqlite3_int64)(now - (time_t)USAGE_RETENTION_DAYS * 24 * 60 * 60));
        if (sqlite3_step(summary) == SQLITE_ROW) {
            total = sqlite3_column_int64(summary, 0);
            last_minute = sqlite3_column_int64(summary, 1);
            last_hour = sqlite3_column_int64(summary, 2);
            errors = sqlite3_column_int64(summary, 3);
            last_called = sqlite3_column_int64(summary, 4);
            unattributed = sqlite3_column_int64(summary, 5);
        }
    }
    sqlite3_finalize(summary);

    sqlite3_stmt *file_count = NULL;
    if (sqlite3_prepare_v2(store->db,
                           "SELECT COUNT(*) FROM (SELECT 1 FROM usage_event_files f "
                           "JOIN usage_events e ON e.id=f.event_id WHERE e.ts>=?1 "
                           "GROUP BY f.project,f.file_path)",
                           -1, &file_count, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(file_count, 1,
                           (sqlite3_int64)(now - (time_t)USAGE_RETENTION_DAYS * 24 * 60 * 60));
        if (sqlite3_step(file_count) == SQLITE_ROW) {
            active_files = sqlite3_column_int64(file_count, 0);
        }
    }
    sqlite3_finalize(file_count);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "available", true);
    yyjson_mut_obj_add_int(doc, root, "retention_days", USAGE_RETENTION_DAYS);
    yyjson_mut_obj_add_sint(doc, root, "total_calls", total);
    yyjson_mut_obj_add_sint(doc, root, "calls_last_minute", last_minute);
    yyjson_mut_obj_add_sint(doc, root, "calls_last_hour", last_hour);
    yyjson_mut_obj_add_sint(doc, root, "errors", errors);
    yyjson_mut_obj_add_sint(doc, root, "active_files", active_files);
    yyjson_mut_obj_add_sint(doc, root, "unattributed_calls", unattributed);
    yyjson_mut_obj_add_sint(doc, root, "last_called_at_ms", last_called * 1000);

    yyjson_mut_val *files = yyjson_mut_arr(doc);
    static const char files_sql[] =
        "SELECT f.project,f.file_path,COUNT(*),COALESCE(SUM(e.ts>=?1),0),MAX(e.ts),"
        "(SELECT e2.tool FROM usage_event_files f2 JOIN usage_events e2 ON e2.id=f2.event_id "
        " WHERE f2.project=f.project AND f2.file_path=f.file_path AND e2.ts>=?3 "
        " GROUP BY e2.tool ORDER BY COUNT(*) DESC,e2.tool LIMIT 1) "
        "FROM usage_event_files f JOIN usage_events e ON e.id=f.event_id "
        "WHERE e.ts>=?3 "
        "GROUP BY f.project,f.file_path "
        "ORDER BY COALESCE(SUM(e.ts>=?1),0) DESC,COUNT(*) DESC,MAX(e.ts) DESC LIMIT ?2";
    sqlite3_stmt *rows = NULL;
    if (sqlite3_prepare_v2(store->db, files_sql, -1, &rows, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(rows, 1, (sqlite3_int64)(now - 3600));
        sqlite3_bind_int(rows, 2, file_limit);
        sqlite3_bind_int64(rows, 3,
                           (sqlite3_int64)(now - (time_t)USAGE_RETENTION_DAYS * 24 * 60 * 60));
        while (sqlite3_step(rows) == SQLITE_ROW) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            const char *project = (const char *)sqlite3_column_text(rows, 0);
            const char *file_path = (const char *)sqlite3_column_text(rows, 1);
            const char *primary_tool = (const char *)sqlite3_column_text(rows, 5);
            yyjson_mut_obj_add_strcpy(doc, item, "project", project ? project : "");
            yyjson_mut_obj_add_strcpy(doc, item, "file_path", file_path ? file_path : "");
            yyjson_mut_obj_add_sint(doc, item, "calls", sqlite3_column_int64(rows, 2));
            yyjson_mut_obj_add_sint(doc, item, "calls_last_hour", sqlite3_column_int64(rows, 3));
            yyjson_mut_obj_add_sint(doc, item, "last_called_at_ms",
                                    sqlite3_column_int64(rows, 4) * 1000);
            yyjson_mut_obj_add_strcpy(doc, item, "primary_tool", primary_tool ? primary_tool : "");
            yyjson_mut_arr_add_val(files, item);
        }
    }
    sqlite3_finalize(rows);
    yyjson_mut_obj_add_val(doc, root, "files", files);

    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    cbm_usage_store_close(store);
    return json ? json : usage_empty_json(false);
}
