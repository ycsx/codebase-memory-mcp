/*
 * pass_cross_repo.c — Cross-repo intelligence: match Routes, Channels, and
 * async topics across indexed projects to create CROSS_* edges.
 *
 * For each HTTP_CALLS/ASYNC_CALLS edge in the source project, looks up the
 * target Route QN in other project DBs. For each Channel node with EMITS
 * edges, looks for matching LISTENS_ON in other projects (and vice versa).
 *
 * Edges are written bidirectionally: both source and target project DBs
 * get a CROSS_* edge so the link is visible from either side.
 */
#include "pipeline/pass_cross_repo.h"
#include "pipeline/pipeline_internal.h" // cbm_route_canon_path
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/str_util.h"

#include <sqlite3/sqlite3.h>
#include <yyjson/yyjson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    CR_PATH_BUF = 1024,
    CR_QN_BUF = 512,
    CR_PROPS_BUF = CBM_SZ_8K,
    CR_MAX_EDGES = 4096,
    CR_DB_EXT_LEN = 3, /* strlen(".db") */
    CR_INIT_CAP = 32,
    CR_COL_3 = 3,
    CR_COL_4 = 4,
    CR_SCHEME_SKIP = 3,      /* strlen("://") */
    CR_ROUTE_PREFIX_LEN = 9, /* strlen("__route__") */
    CR_ANY_LEN = 3,          /* strlen("ANY") */
    CR_PACKAGE_MAX = 512,
    CR_PACKAGE_DEPTH = 8,
    CR_MANIFEST_BUF = CBM_SZ_64K,
    CR_TARGET_MAX = 256,
};

#define CR_MS_PER_SEC 1000.0
#define CR_NS_PER_MS 1000000.0

/* TLS buffer for integer-to-string in log calls. */
static CBM_TLS char cr_ibuf[CBM_SZ_32];
static const char *cr_itoa(int v) {
    snprintf(cr_ibuf, sizeof(cr_ibuf), "%d", v);
    return cr_ibuf;
}

typedef struct {
    char name[CBM_SZ_256];
    char rel_dir[CR_PATH_BUF];
    char entry_rel[CR_PATH_BUF];
} cr_package_t;

typedef struct {
    cr_package_t items[CR_PACKAGE_MAX];
    int count;
} cr_package_list_t;

/* ── Helpers ─────────────────────────────────────────────────────── */

static const char *cr_cache_dir(void) {
    const char *dir = cbm_resolve_cache_dir();
    return dir ? dir : cbm_tmpdir();
}

static void cr_db_path(const char *project, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%s/%s.db", cr_cache_dir(), project);
}

/* Extract a JSON string property from properties_json.
 * Writes into buf, returns buf on success, NULL on miss. */
static const char *json_str_prop(const char *json, const char *key, char *buf, size_t bufsz) {
    if (!json || !key || !buf || bufsz == 0) {
        return NULL;
    }
    char pat[CBM_SZ_128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *cursor = json;
    while ((cursor = strstr(cursor, pat)) != NULL) {
        const char *start = cursor + strlen(pat);
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
            start++;
        }
        if (*start++ != ':') {
            cursor += strlen(pat);
            continue;
        }
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
            start++;
        }
        if (*start++ != '"') {
            cursor += strlen(pat);
            continue;
        }
        size_t n = 0;
        for (const char *p = start; *p; p++) {
            if (*p == '"') {
                buf[n] = '\0';
                return buf;
            }
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            if (n + 1 < bufsz) {
                buf[n++] = *p;
            }
        }
        return NULL;
    }
    return NULL;
}

static void cr_slash_normalize(char *path) {
    if (!path) {
        return;
    }
    for (char *p = path; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    while (path[0] == '.' && path[1] == '/') {
        memmove(path, path + 2, strlen(path + 2) + 1U);
    }
    while (strstr(path, "/./")) {
        char *p = strstr(path, "/./");
        memmove(p, p + 2, strlen(p + 2) + 1U);
    }
}

static void cr_join_rel(const char *base, const char *suffix, char *out, size_t outsz) {
    if (!suffix) {
        suffix = "";
    }
    snprintf(out, outsz, "%s%s%s", base && base[0] ? base : "", base && base[0] ? "/" : "", suffix);
    cr_slash_normalize(out);
}

/* Resolve a package.json exports value without treating the manifest as an
 * unstructured string. Conditional exports are ordered by runtime preference;
 * arrays use the first resolvable target, as specified by Node.js. */
static const char *cr_resolve_exports_value(yyjson_val *value, int depth) {
    if (!value || depth > CR_PACKAGE_DEPTH) {
        return NULL;
    }
    if (yyjson_is_str(value)) {
        return yyjson_get_str(value);
    }
    if (yyjson_is_arr(value)) {
        size_t index, max;
        yyjson_val *item;
        yyjson_arr_foreach(value, index, max, item) {
            const char *resolved = cr_resolve_exports_value(item, depth + 1);
            if (resolved && resolved[0]) {
                return resolved;
            }
        }
        return NULL;
    }
    if (!yyjson_is_obj(value)) {
        return NULL;
    }

    /* Prefer the same conditions used by the package map pass. Browser and
     * node are included for packages that publish platform-specific entries. */
    static const char *const conditions[] = {"import", "require", "browser",
                                             "node",   "default", "types"};
    for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); i++) {
        const char *resolved =
            cr_resolve_exports_value(yyjson_obj_get(value, conditions[i]), depth + 1);
        if (resolved && resolved[0]) {
            return resolved;
        }
    }

    /* Unknown condition names (for example "development") are valid. Do not
     * mistake a subpath export such as "./cli" for the package root entry. */
    yyjson_obj_iter iter = yyjson_obj_iter_with(value);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *name = yyjson_get_str(key);
        if (name && name[0] == '.') {
            continue;
        }
        const char *resolved = cr_resolve_exports_value(yyjson_obj_iter_get_val(key), depth + 1);
        if (resolved && resolved[0]) {
            return resolved;
        }
    }
    return NULL;
}

static bool cr_read_package_manifest(const char *abs_path, const char *rel_dir, cr_package_t *out) {
    FILE *f = cbm_fopen(abs_path, "rb");
    if (!f) {
        return false;
    }
    char source[CR_MANIFEST_BUF];
    size_t len = fread(source, 1, sizeof(source) - 1U, f);
    fclose(f);
    source[len] = '\0';
    yyjson_doc *doc = yyjson_read(source, len, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *name_val = yyjson_is_obj(root) ? yyjson_obj_get(root, "name") : NULL;
    const char *name = yyjson_is_str(name_val) ? yyjson_get_str(name_val) : NULL;
    if (!name || !name[0]) {
        yyjson_doc_free(doc);
        return false;
    }
    const char *entry = NULL;
    if (yyjson_is_obj(root)) {
        yyjson_val *exports = yyjson_obj_get(root, "exports");
        if (yyjson_is_obj(exports)) {
            entry = cr_resolve_exports_value(yyjson_obj_get(exports, "."), 0);
            if (!entry) {
                entry = cr_resolve_exports_value(exports, 0);
            }
        } else {
            entry = cr_resolve_exports_value(exports, 0);
        }
        if (!entry) {
            static const char *const fallback_keys[] = {"module", "main",    "browser",
                                                        "import", "require", "types"};
            for (size_t i = 0; i < sizeof(fallback_keys) / sizeof(fallback_keys[0]); i++) {
                yyjson_val *candidate = yyjson_obj_get(root, fallback_keys[i]);
                if (yyjson_is_str(candidate)) {
                    entry = yyjson_get_str(candidate);
                    break;
                }
            }
        }
    }
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->rel_dir, sizeof(out->rel_dir), "%s", rel_dir ? rel_dir : "");
    snprintf(out->entry_rel, sizeof(out->entry_rel), "%s",
             entry && entry[0] ? entry : "src/index.ts");
    cr_slash_normalize(out->entry_rel);
    while (out->entry_rel[0] == '/') {
        memmove(out->entry_rel, out->entry_rel + 1, strlen(out->entry_rel));
    }
    yyjson_doc_free(doc);
    return true;
}

static void cr_scan_packages(const char *root, const char *rel_dir, int depth,
                             cr_package_list_t *out) {
    if (!root || !out || out->count >= CR_PACKAGE_MAX || depth > CR_PACKAGE_DEPTH) {
        return;
    }
    char dir_path[CR_PATH_BUF];
    if (rel_dir && rel_dir[0]) {
        snprintf(dir_path, sizeof(dir_path), "%s/%s", root, rel_dir);
    } else {
        snprintf(dir_path, sizeof(dir_path), "%s", root);
    }
    char manifest[CR_PATH_BUF];
    snprintf(manifest, sizeof(manifest), "%s/package.json", dir_path);
    cr_package_t pkg = {0};
    if (cr_read_package_manifest(manifest, rel_dir, &pkg)) {
        bool duplicate = false;
        for (int i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].name, pkg.name) == 0 &&
                strcmp(out->items[i].rel_dir, pkg.rel_dir) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && out->count < CR_PACKAGE_MAX) {
            out->items[out->count++] = pkg;
        }
    }
    cbm_dir_t *dir = cbm_opendir(dir_path);
    if (!dir) {
        return;
    }
    cbm_dirent_t *ent;
    while (out->count < CR_PACKAGE_MAX && (ent = cbm_readdir(dir)) != NULL) {
        if (!ent->is_dir || ent->is_symlink || strcmp(ent->name, ".") == 0 ||
            strcmp(ent->name, "..") == 0 || strcmp(ent->name, "node_modules") == 0 ||
            strcmp(ent->name, ".git") == 0) {
            continue;
        }
        char child_rel[CR_PATH_BUF];
        cr_join_rel(rel_dir, ent->name, child_rel, sizeof(child_rel));
        cr_scan_packages(root, child_rel, depth + 1, out);
    }
    cbm_closedir(dir);
}

static const cr_package_t *cr_find_package(const cr_package_list_t *packages, const char *name) {
    if (!packages || !name || !name[0]) {
        return NULL;
    }
    for (int i = 0; i < packages->count; i++) {
        if (strcmp(packages->items[i].name, name) == 0) {
            return &packages->items[i];
        }
    }
    return NULL;
}

static const cr_package_t *cr_find_package_dir(const cr_package_list_t *packages,
                                               const char *rel_dir) {
    if (!packages || !rel_dir) {
        return NULL;
    }
    char normalized[CR_PATH_BUF];
    snprintf(normalized, sizeof(normalized), "%s", rel_dir);
    cr_slash_normalize(normalized);
    for (int i = 0; i < packages->count; i++) {
        if (strcmp(packages->items[i].rel_dir, normalized) == 0) {
            return &packages->items[i];
        }
    }
    return NULL;
}

static void cr_collapse_relative(char *path) {
    char *parts[CR_PATH_BUF / 2];
    int count = 0;
    char work[CR_PATH_BUF];
    snprintf(work, sizeof(work), "%s", path ? path : "");
    cr_slash_normalize(work);
    char *part = work;
    while (*part && count < (int)(sizeof(parts) / sizeof(parts[0]))) {
        while (*part == '/') {
            part++;
        }
        if (!*part) {
            break;
        }
        char *end = strchr(part, '/');
        if (end) {
            *end = '\0';
        }
        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            part = end ? end + 1 : part + strlen(part);
            continue;
        }
        if (strcmp(part, "..") == 0) {
            if (count > 0) {
                count--;
            }
        } else {
            parts[count++] = part;
        }
        part = end ? end + 1 : part + strlen(part);
    }
    path[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            strncat(path, "/", CR_PATH_BUF - strlen(path) - 1U);
        }
        strncat(path, parts[i], CR_PATH_BUF - strlen(path) - 1U);
    }
}

static int64_t cr_find_file_node(cbm_store_t *store, const char *project, const char *rel_path,
                                 char *file_out, size_t file_sz) {
    if (!store || !project || !rel_path) {
        return 0;
    }
    struct sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return 0;
    }
    static const char *const suffixes[] = {"", ".js", ".ts", ".jsx", ".tsx", ".vue", NULL};
    for (int i = 0; suffixes[i]; i++) {
        char candidate[CR_PATH_BUF];
        snprintf(candidate, sizeof(candidate), "%s%s", rel_path, suffixes[i]);
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT id,file_path FROM nodes WHERE project=?1 AND file_path=?2 "
                               "ORDER BY id LIMIT 1",
                               CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_text(stmt, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_STATIC);
        sqlite3_bind_text(stmt, PAIR_LEN, candidate, CBM_NOT_FOUND, SQLITE_TRANSIENT);
        int64_t id = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int64(stmt, 0);
            if (file_out && file_sz > 0) {
                const char *file = (const char *)sqlite3_column_text(stmt, SKIP_ONE);
                snprintf(file_out, file_sz, "%s", file ? file : candidate);
            }
        }
        sqlite3_finalize(stmt);
        if (id != 0) {
            return id;
        }
    }
    return 0;
}

static bool cr_find_node_qn(cbm_store_t *store, const char *project, int64_t node_id, char *out,
                            size_t out_sz) {
    if (!store || !project || node_id <= 0 || !out || out_sz == 0) {
        return false;
    }
    struct sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    if (!db || sqlite3_prepare_v2(db, "SELECT qualified_name FROM nodes WHERE project=?1 AND id=?2",
                                  -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, node_id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(stmt, 0);
        if (qn && qn[0]) {
            snprintf(out, out_sz, "%s", qn);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static int64_t cr_find_project_node(cbm_store_t *store, const char *project) {
    struct sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !project) {
        return 0;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM nodes WHERE project=?1 AND label='Project' LIMIT 1",
                           CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_STATIC);
    int64_t id = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return id;
}

static const char *cr_package_name_from_spec(const char *spec, char *name_out, size_t name_sz,
                                             const char **subpath_out) {
    if (!spec || !name_out || name_sz == 0) {
        return NULL;
    }
    const char *start = spec;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (strncmp(start, "workspace:", 10) == 0 || strncmp(start, "file:", 5) == 0 ||
        strncmp(start, "link:", 5) == 0) {
        return NULL;
    }
    const char *slash = strchr(start, '/');
    const char *end = slash;
    if (start[0] == '@' && slash) {
        slash = strchr(slash + 1, '/');
        end = slash;
    }
    if (!end) {
        end = start + strlen(start);
    }
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= name_sz) {
        return NULL;
    }
    memcpy(name_out, start, len);
    name_out[len] = '\0';
    if (subpath_out) {
        *subpath_out = end[0] == '/' ? end + 1 : NULL;
    }
    return name_out;
}

static const cr_package_t *cr_source_package(const cr_package_list_t *packages,
                                             const char *source_file) {
    const cr_package_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; packages && i < packages->count; i++) {
        const char *dir = packages->items[i].rel_dir;
        size_t len = strlen(dir);
        const char *file = source_file ? source_file : "";
        size_t file_len = strlen(file);
        bool match = len == 0 || (file_len >= len && strncmp(file, dir, len) == 0 &&
                                  (file[len] == '/' || file[len] == '\0'));
        if (match && len >= best_len) {
            best = &packages->items[i];
            best_len = len;
        }
    }
    return best;
}

static void insert_cross_edge(cbm_store_t *store, const char *project, int64_t from_id,
                              int64_t to_id, const char *edge_type, const char *props);

static int cr_match_package_imports(cbm_store_t *src_store, const char *src_project,
                                    cbm_store_t *tgt_store, const char *tgt_project,
                                    const cr_package_list_t *src_packages,
                                    const cr_package_list_t *tgt_packages) {
    struct sqlite3 *db = cbm_store_get_db(src_store);
    if (!db) {
        return 0;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT e.source_id,e.target_id,e.properties,n.file_path "
            "FROM edges e JOIN nodes n ON n.id=e.source_id "
            "WHERE e.project=?1 AND e.type='IMPORTS' ORDER BY e.source_id,e.target_id",
            CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);
    int created = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t source_id = sqlite3_column_int64(stmt, 0);
        const char *props = (const char *)sqlite3_column_text(stmt, 2);
        const char *source_file = (const char *)sqlite3_column_text(stmt, 3);
        char spec[CBM_SZ_512] = {0};
        if (!json_str_prop(props, "module_path", spec, sizeof(spec)) &&
            !json_str_prop(props, "specifier", spec, sizeof(spec))) {
            continue;
        }
        const cr_package_t *source_pkg = cr_source_package(src_packages, source_file);
        const cr_package_t *target_pkg = NULL;
        const char *subpath = NULL;
        char package_name[CBM_SZ_256] = {0};
        char target_rel[CR_PATH_BUF] = {0};
        if (strncmp(spec, "file:", 5) == 0 || strncmp(spec, "link:", 5) == 0 ||
            strncmp(spec, "workspace:", 10) == 0) {
            const char *path_spec = strchr(spec, ':') + 1;
            while (*path_spec == '/') {
                path_spec++;
            }
            if (strcmp(path_spec, "*") == 0 || strcmp(path_spec, "^") == 0) {
                continue;
            }
            cr_join_rel(source_pkg ? source_pkg->rel_dir : "", path_spec, target_rel,
                        sizeof(target_rel));
            cr_collapse_relative(target_rel);
            target_pkg = cr_find_package_dir(tgt_packages, target_rel);
            if (!target_pkg && strncmp(spec, "workspace:", 10) == 0) {
                target_pkg = cr_find_package(tgt_packages, path_spec);
            }
        } else if (cr_package_name_from_spec(spec, package_name, sizeof(package_name), &subpath)) {
            target_pkg = cr_find_package(tgt_packages, package_name);
        }
        if (!target_pkg) {
            continue;
        }
        if (subpath && subpath[0]) {
            cr_join_rel(target_pkg->rel_dir, subpath, target_rel, sizeof(target_rel));
        } else {
            cr_join_rel(target_pkg->rel_dir, target_pkg->entry_rel, target_rel, sizeof(target_rel));
        }
        cr_collapse_relative(target_rel);
        char target_file[CR_PATH_BUF] = {0};
        int64_t target_id =
            cr_find_file_node(tgt_store, tgt_project, target_rel, target_file, sizeof(target_file));
        if (target_id == 0) {
            continue;
        }
        char source_qn[CR_QN_BUF] = {0};
        char target_qn[CR_QN_BUF] = {0};
        (void)cr_find_node_qn(src_store, src_project, source_id, source_qn, sizeof(source_qn));
        (void)cr_find_node_qn(tgt_store, tgt_project, target_id, target_qn, sizeof(target_qn));
        char esc_package[CBM_SZ_512];
        char esc_spec[CBM_SZ_1K];
        char esc_src_project[CBM_SZ_512];
        char esc_tgt_project[CBM_SZ_512];
        char esc_source_qn[CR_QN_BUF * 2];
        char esc_target_qn[CR_QN_BUF * 2];
        char esc_source_file[CR_PATH_BUF];
        char esc_target_file[CR_PATH_BUF];
        cbm_json_escape(esc_package, sizeof(esc_package), target_pkg->name);
        cbm_json_escape(esc_spec, sizeof(esc_spec), spec);
        cbm_json_escape(esc_src_project, sizeof(esc_src_project), src_project);
        cbm_json_escape(esc_tgt_project, sizeof(esc_tgt_project), tgt_project);
        cbm_json_escape(esc_source_qn, sizeof(esc_source_qn), source_qn);
        cbm_json_escape(esc_target_qn, sizeof(esc_target_qn), target_qn);
        cbm_json_escape(esc_source_file, sizeof(esc_source_file), source_file ? source_file : "");
        cbm_json_escape(esc_target_file, sizeof(esc_target_file), target_file);
        char props_buf[CR_PROPS_BUF];
        snprintf(props_buf, sizeof(props_buf),
                 "{\"package\":\"%s\",\"specifier\":\"%s\","
                 "\"source_project\":\"%s\",\"target_project\":\"%s\","
                 "\"source_file\":\"%s\",\"target_file\":\"%s\","
                 "\"target_qualified_name\":\"%s\"}",
                 esc_package, esc_spec, esc_src_project, esc_tgt_project, esc_source_file,
                 esc_target_file, esc_target_qn);
        insert_cross_edge(src_store, src_project, source_id, target_id, "CROSS_PACKAGE_IMPORTS",
                          props_buf);

        /* The reverse dependency edge is stored in the target DB. Keep its
         * target identity oriented from that project's perspective so the UI
         * can resolve the satellite endpoint using the same metadata contract. */
        char reverse_props[CR_PROPS_BUF];
        snprintf(reverse_props, sizeof(reverse_props),
                 "{\"package\":\"%s\",\"specifier\":\"%s\","
                 "\"source_project\":\"%s\",\"target_project\":\"%s\","
                 "\"source_file\":\"%s\",\"target_file\":\"%s\","
                 "\"target_qualified_name\":\"%s\"}",
                 esc_package, esc_spec, esc_tgt_project, esc_src_project, esc_target_file,
                 esc_source_file, esc_source_qn);
        insert_cross_edge(tgt_store, tgt_project, target_id, source_id, "CROSS_PROJECT_DEPENDS",
                          reverse_props);
        created++;
    }
    sqlite3_finalize(stmt);
    return created;
}

/* Match local protocols declared in package.json even when no source import
 * was indexed (for example a package re-exported by generated code). */
static int cr_match_manifest_dependencies(cbm_store_t *src_store, const char *src_project,
                                          const char *src_root, cbm_store_t *tgt_store,
                                          const char *tgt_project,
                                          const cr_package_list_t *src_packages,
                                          const cr_package_list_t *tgt_packages) {
    int created = 0;
    static const char *const sections[] = {"dependencies", "devDependencies", "peerDependencies",
                                           "optionalDependencies"};
    for (int pi = 0; src_packages && pi < src_packages->count; pi++) {
        const cr_package_t *src_pkg = &src_packages->items[pi];
        char manifest_path[CR_PATH_BUF];
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s%s/package.json", src_root,
                 src_pkg->rel_dir[0] ? src_pkg->rel_dir : "", src_pkg->rel_dir[0] ? "/" : "");
        FILE *f = cbm_fopen(manifest_path, "rb");
        if (!f) {
            continue;
        }
        char source[CR_MANIFEST_BUF];
        size_t len = fread(source, 1, sizeof(source) - 1U, f);
        fclose(f);
        source[len] = '\0';
        yyjson_doc *doc = yyjson_read(source, len, 0);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        if (!yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            continue;
        }
        char manifest_rel[CR_PATH_BUF];
        snprintf(manifest_rel, sizeof(manifest_rel), "%s%s",
                 src_pkg->rel_dir[0] ? src_pkg->rel_dir : "",
                 src_pkg->rel_dir[0] ? "/package.json" : "package.json");
        int64_t source_id = cr_find_file_node(src_store, src_project, manifest_rel, NULL, 0);
        if (source_id == 0) {
            /* package.json is intentionally excluded from normal source files
             * in some index modes; keep the dependency visible from Project. */
            source_id = cr_find_project_node(src_store, src_project);
            if (source_id == 0) {
                yyjson_doc_free(doc);
                continue;
            }
        }
        for (size_t si = 0; si < sizeof(sections) / sizeof(sections[0]); si++) {
            yyjson_val *deps = yyjson_obj_get(root, sections[si]);
            if (!yyjson_is_obj(deps)) {
                continue;
            }
            yyjson_obj_iter it = yyjson_obj_iter_with(deps);
            yyjson_val *key;
            while ((key = yyjson_obj_iter_next(&it)) != NULL) {
                yyjson_val *value = yyjson_obj_iter_get_val(key);
                if (!yyjson_is_str(key) || !yyjson_is_str(value)) {
                    continue;
                }
                const char *dep_name = yyjson_get_str(key);
                const char *spec = yyjson_get_str(value);
                if (!dep_name || !spec ||
                    !(strncmp(spec, "file:", 5) == 0 || strncmp(spec, "link:", 5) == 0 ||
                      strncmp(spec, "workspace:", 10) == 0)) {
                    continue;
                }
                const char *path_spec = strchr(spec, ':') + 1;
                while (*path_spec == '/') {
                    path_spec++;
                }
                char target_rel[CR_PATH_BUF];
                cr_join_rel(src_pkg->rel_dir, path_spec, target_rel, sizeof(target_rel));
                cr_collapse_relative(target_rel);
                const cr_package_t *target_pkg = cr_find_package_dir(tgt_packages, target_rel);
                if (!target_pkg) {
                    target_pkg = cr_find_package(tgt_packages, dep_name);
                }
                if (!target_pkg) {
                    continue;
                }
                cr_join_rel(target_pkg->rel_dir, target_pkg->entry_rel, target_rel,
                            sizeof(target_rel));
                cr_collapse_relative(target_rel);
                int64_t target_id = cr_find_file_node(tgt_store, tgt_project, target_rel, NULL, 0);
                if (target_id == 0) {
                    continue;
                }
                char source_qn[CR_QN_BUF] = {0};
                char target_qn[CR_QN_BUF] = {0};
                (void)cr_find_node_qn(src_store, src_project, source_id, source_qn,
                                      sizeof(source_qn));
                (void)cr_find_node_qn(tgt_store, tgt_project, target_id, target_qn,
                                      sizeof(target_qn));
                char esc_pkg[CBM_SZ_512], esc_spec[CBM_SZ_1K], esc_src[CBM_SZ_512],
                    esc_tgt[CBM_SZ_512], esc_source_file[CR_PATH_BUF], esc_target_file[CR_PATH_BUF],
                    esc_source_qn[CR_QN_BUF * 2], esc_target_qn[CR_QN_BUF * 2];
                cbm_json_escape(esc_pkg, sizeof(esc_pkg), target_pkg->name);
                cbm_json_escape(esc_spec, sizeof(esc_spec), spec);
                cbm_json_escape(esc_src, sizeof(esc_src), src_project);
                cbm_json_escape(esc_tgt, sizeof(esc_tgt), tgt_project);
                cbm_json_escape(esc_source_qn, sizeof(esc_source_qn), source_qn);
                cbm_json_escape(esc_target_qn, sizeof(esc_target_qn), target_qn);
                cbm_json_escape(esc_source_file, sizeof(esc_source_file), manifest_rel);
                cbm_json_escape(esc_target_file, sizeof(esc_target_file), target_rel);
                char props[CR_PROPS_BUF];
                snprintf(props, sizeof(props),
                         "{\"package\":\"%s\",\"specifier\":\"%s\","
                         "\"source_project\":\"%s\",\"target_project\":\"%s\","
                         "\"source_file\":\"%s\",\"target_file\":\"%s\","
                         "\"target_qualified_name\":\"%s\","
                         "\"resolution\":\"manifest\"}",
                         esc_pkg, esc_spec, esc_src, esc_tgt, esc_source_file, esc_target_file,
                         esc_target_qn);
                insert_cross_edge(src_store, src_project, source_id, target_id,
                                  "CROSS_PACKAGE_IMPORTS", props);
                char reverse_props[CR_PROPS_BUF];
                snprintf(reverse_props, sizeof(reverse_props),
                         "{\"package\":\"%s\",\"specifier\":\"%s\","
                         "\"source_project\":\"%s\",\"target_project\":\"%s\","
                         "\"source_file\":\"%s\",\"target_file\":\"%s\","
                         "\"target_qualified_name\":\"%s\","
                         "\"resolution\":\"manifest\"}",
                         esc_pkg, esc_spec, esc_tgt, esc_src, esc_target_file, esc_source_file,
                         esc_source_qn);
                insert_cross_edge(tgt_store, tgt_project, target_id, source_id,
                                  "CROSS_PROJECT_DEPENDS", reverse_props);
                created++;
            }
        }
        yyjson_doc_free(doc);
    }
    return created;
}

/* Build CROSS_* edge properties JSON. */
static void build_cross_props(char *buf, size_t bufsz, const char *target_project,
                              const char *target_function, const char *target_file,
                              const char *url_or_channel, const char *extra_key,
                              const char *extra_val) {
    if (!buf || bufsz == 0) {
        return;
    }
    char esc_project[CR_PATH_BUF * 2];
    char esc_function[CR_PATH_BUF * 2];
    char esc_file[CR_PATH_BUF * 2];
    char esc_value[CR_PATH_BUF * 2];
    cbm_json_escape(esc_project, (int)sizeof(esc_project), target_project ? target_project : "");
    cbm_json_escape(esc_function, (int)sizeof(esc_function),
                    target_function ? target_function : "");
    cbm_json_escape(esc_file, (int)sizeof(esc_file), target_file ? target_file : "");
    cbm_json_escape(esc_value, (int)sizeof(esc_value), url_or_channel ? url_or_channel : "");
    int n = snprintf(buf, bufsz,
                     "{\"target_project\":\"%s\",\"target_function\":\"%s\","
                     "\"target_file\":\"%s\"",
                     esc_project, esc_function, esc_file);
    if (n < 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return;
    }
    if (url_or_channel && url_or_channel[0]) {
        int written = snprintf(buf + n, bufsz - (size_t)n, ",\"%s\":\"%s\"",
                               extra_key ? extra_key : "url_path", esc_value);
        if (written < 0 || (size_t)written >= bufsz - (size_t)n) {
            buf[0] = '\0';
            return;
        }
        n += written;
    }
    if (extra_val && extra_val[0]) {
        char esc_extra[CR_PATH_BUF * 2];
        cbm_json_escape(esc_extra, (int)sizeof(esc_extra), extra_val);
        const char *secondary_key =
            extra_key && strcmp(extra_key, "channel_name") == 0 ? "transport" : "method";
        int written =
            snprintf(buf + n, bufsz - (size_t)n, ",\"%s\":\"%s\"", secondary_key, esc_extra);
        if (written < 0 || (size_t)written >= bufsz - (size_t)n) {
            buf[0] = '\0';
            return;
        }
        n += written;
    }
    if ((size_t)n + 2U > bufsz) {
        buf[0] = '\0';
        return;
    }
    memcpy(buf + n, "}", 2U);
}

/* Delete all CROSS_* edges for a project from a store. */
static void delete_cross_edges(cbm_store_t *store, const char *project) {
    cbm_store_delete_edges_by_type(store, project, "CROSS_HTTP_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_ASYNC_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_CHANNEL");
    cbm_store_delete_edges_by_type(store, project, "CROSS_GRPC_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_GRAPHQL_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_TRPC_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_PACKAGE_IMPORTS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_PROJECT_DEPENDS");
}

/* Remove only the reverse edges previously emitted by `source_project` from a
 * target database. Re-indexing one project must not delete cross-repo edges
 * belonging to unrelated sources. */
static void delete_reverse_edges_for_source(cbm_store_t *store, const char *target_project,
                                            const char *source_project) {
    struct sqlite3 *db = cbm_store_get_db(store);
    if (!db || !target_project || !source_project) {
        return;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM edges WHERE project = ?1 AND type LIKE 'CROSS_%' "
                      "AND json_valid(properties) "
                      "AND json_extract(properties, '$.source_project') = ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, target_project, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, source_project, -1, SQLITE_STATIC);
    (void)sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static int collect_all_projects(char ***out);
static void free_project_list(char **projects, int count);

/* Reverse edges live in every target DB, so clear the source's old reverse
 * records before rebuilding its current relationships. */
static void clear_reverse_edges_for_source(const char *source_project) {
    char **projects = NULL;
    int count = collect_all_projects(&projects);
    for (int i = 0; i < count; i++) {
        if (!projects[i] || strcmp(projects[i], source_project) == 0) {
            continue;
        }
        char path[CR_PATH_BUF];
        cr_db_path(projects[i], path, sizeof(path));
        cbm_store_t *store = cbm_store_open_path(path);
        if (store) {
            delete_reverse_edges_for_source(store, projects[i], source_project);
            cbm_store_close(store);
        }
    }
    free_project_list(projects, count);
}

/* Insert a CROSS_* edge into a store. Idempotent by construction: the edges
 * table is UNIQUE(source_id, target_id, type) and cbm_store_insert_edge
 * upserts on conflict, so a pair reached from both match directions or on a
 * repeat run never duplicates a row. (#523) */
static void insert_cross_edge(cbm_store_t *store, const char *project, int64_t from_id,
                              int64_t to_id, const char *edge_type, const char *props) {
    cbm_edge_t edge = {
        .project = project,
        .source_id = from_id,
        .target_id = to_id,
        .type = edge_type,
        .properties_json = props,
    };
    cbm_store_insert_edge(store, &edge);
}

/* Strip "scheme://host[:port]" from a stored HTTP_CALLS url, returning the
 * path. url_path property values are stored raw from the call's first string
 * argument, so they can be full URLs ("scheme://host:port/v2/x") — and
 * cbm_route_canon_path only canonicalizes placeholder syntax, never strips
 * authorities. Returns "/" for a URL with no path after the host (a request
 * against the bare base URL targets the root route). (#523) */
static const char *cr_url_path(const char *url) {
    if (!url) {
        return url;
    }
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return url; /* already a bare path */
    }
    const char *path_start = strchr(scheme_end + CR_SCHEME_SKIP, '/');
    return path_start ? path_start : "/";
}

/* Rewrite known public gateway prefixes to the provider-side route prefix.
 * Only apply these
 * aliases after a direct match misses: callers keep their
 * original URL in edge properties, while
 * route lookup uses the internal path.
 * The segment-boundary check prevents prefixes such as
 * "/api/publication"
 * from being rewritten accidentally. */
static bool cr_gateway_alias_path(const char *path, char *buf, size_t bufsz) {
    static const struct {
        const char *public_prefix;
        const char *provider_prefix;
    } aliases[] = {
        {"/api/public", "/api/nwgpt"},
    };

    if (!path || !buf || bufsz == 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        size_t prefix_len = strlen(aliases[i].public_prefix);
        if (strncmp(path, aliases[i].public_prefix, prefix_len) != 0 ||
            (path[prefix_len] != '\0' && path[prefix_len] != '/')) {
            continue;
        }
        int n = snprintf(buf, bufsz, "%s%s", aliases[i].provider_prefix, path + prefix_len);
        return n >= 0 && (size_t)n < bufsz;
    }
    return false;
}

/* Look up a node's name and file_path by id. */
static void lookup_node_info(struct sqlite3 *db, int64_t node_id, char *name_out, size_t name_sz,
                             char *file_out, size_t file_sz) {
    name_out[0] = '\0';
    file_out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name, file_path FROM nodes WHERE id = ?1", CBM_NOT_FOUND,
                           &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(st, SKIP_ONE, node_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(st, 0);
        const char *fp = (const char *)sqlite3_column_text(st, SKIP_ONE);
        if (nm) {
            snprintf(name_out, name_sz, "%s", nm);
        }
        if (fp) {
            snprintf(file_out, file_sz, "%s", fp);
        }
    }
    sqlite3_finalize(st);
}

/* ── Phase A: HTTP Route matching ────────────────────────────────── */

/* Find a Route node in target_store by QN and return the handler function's
 * node id, name, and file_path via HANDLES edges. Returns 0 if not found. */
static int64_t find_route_handler(cbm_store_t *target_store, const char *route_qn,
                                  char *handler_name, size_t name_sz, char *handler_file,
                                  size_t file_sz) {
    handler_name[0] = '\0';
    handler_file[0] = '\0';
    struct sqlite3 *db = cbm_store_get_db(target_store);
    if (!db) {
        return 0;
    }

    /* Find Route node by QN */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT id FROM nodes WHERE qualified_name = ?1 AND label = 'Route' LIMIT 1",
            CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, route_qn, CBM_NOT_FOUND, SQLITE_STATIC);
    int64_t route_id = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        route_id = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    if (route_id == 0) {
        return 0;
    }

    /* Follow HANDLES edge to find the handler function */
    if (sqlite3_prepare_v2(db,
                           "SELECT n.id, n.name, n.file_path FROM edges e "
                           "JOIN nodes n ON n.id = e.source_id "
                           "WHERE e.target_id = ?1 AND e.type = 'HANDLES' LIMIT 1",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(s, SKIP_ONE, route_id);
    int64_t handler_id = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        handler_id = sqlite3_column_int64(s, 0);
        const char *n = (const char *)sqlite3_column_text(s, SKIP_ONE);
        const char *f = (const char *)sqlite3_column_text(s, PAIR_LEN);
        if (n) {
            snprintf(handler_name, name_sz, "%s", n);
        }
        if (f) {
            snprintf(handler_file, file_sz, "%s", f);
        }
    }
    sqlite3_finalize(s);
    return handler_id;
}

/* Segment-wise match of a concrete path against a route template path, where a
 * "{...}" segment in the template matches any single non-empty concrete
 * segment. Both inputs are bare paths (no method prefix, no authority).
 * Leading/trailing slashes are insignificant. Returns true on a full match. */
static bool cr_path_matches_template(const char *concrete, const char *templ) {
    const char *c = concrete;
    const char *t = templ;
    while (*c && *t) {
        if (*c == '/') {
            c++;
        }
        if (*t == '/') {
            t++;
        }
        const char *cseg = c;
        while (*c && *c != '/') {
            c++;
        }
        const char *tseg = t;
        while (*t && *t != '/') {
            t++;
        }
        size_t clen = (size_t)(c - cseg);
        size_t tlen = (size_t)(t - tseg);
        bool t_is_param = (tlen >= PAIR_LEN && tseg[0] == '{' && tseg[tlen - 1] == '}');
        if (!t_is_param) {
            if (clen != tlen || strncmp(cseg, tseg, clen) != 0) {
                return false;
            }
        } else if (clen == 0) {
            return false; /* a parameter never matches an empty segment */
        }
    }
    while (*c == '/') {
        c++;
    }
    while (*t == '/') {
        t++;
    }
    return *c == '\0' && *t == '\0';
}

/* Fallback for when the exact route-QN lookup misses: a concrete client path
 * ("/v2/orders/123") never exact-matches a templated route QN
 * ("__route__GET__/v2/orders/{}"). Enumerate the target store's Route nodes
 * and segment-match the concrete path against each template. On a match, copy
 * the route QN into out_qn and return the handler id; returns 0 on no match.
 *
 * COST: this scans every Route node of the target project once per unmatched
 * HTTP_CALLS edge — O(calls × routes) per project pair. Acceptable while both
 * factors stay small (calls are capped at CR_MAX_EDGES and it only runs for
 * edges the exact lookup missed); revisit with a prepared template index if
 * cross-repo matching ever shows up in profiles. (#523) */
static int64_t find_route_handler_fuzzy(cbm_store_t *target_store, const char *concrete_path,
                                        const char *method, char *out_qn, size_t out_qn_sz,
                                        char *handler_name, size_t name_sz, char *handler_file,
                                        size_t file_sz) {
    struct sqlite3 *db = cbm_store_get_db(target_store);
    if (!db || !concrete_path || !concrete_path[0]) {
        return 0;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT qualified_name FROM nodes WHERE label = 'Route'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    int64_t found = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(s, 0);
        if (!qn || strncmp(qn, "__route__", CR_ROUTE_PREFIX_LEN) != 0) {
            continue;
        }
        /* Split "__route__<METHOD>__<path>" */
        const char *rest = qn + CR_ROUTE_PREFIX_LEN;
        const char *sep = strstr(rest, "__");
        if (!sep) {
            continue;
        }
        size_t mlen = (size_t)(sep - rest);
        const char *rpath = sep + PAIR_LEN;
        /* Method gate: the route's method must equal the caller's, or be ANY.
         * A missing caller method matches any route method. */
        if (method && method[0]) {
            bool same_method = (strncmp(rest, method, mlen) == 0 && method[mlen] == '\0');
            bool route_any = (mlen == CR_ANY_LEN && strncmp(rest, "ANY", CR_ANY_LEN) == 0);
            if (!same_method && !route_any) {
                continue;
            }
        }
        if (!cr_path_matches_template(concrete_path, rpath)) {
            continue;
        }
        /* A concrete path can match more than one stored template (e.g. a raw
         * "{id}" variant and its canonical "{}" form). Only accept a Route that
         * actually has a HANDLES edge — the handler is attached to the
         * canonical node. Keep scanning otherwise. */
        int64_t hid =
            find_route_handler(target_store, qn, handler_name, name_sz, handler_file, file_sz);
        if (hid != 0) {
            snprintf(out_qn, out_qn_sz, "%s", qn);
            found = hid;
            break;
        }
    }
    sqlite3_finalize(s);
    return found;
}

/* Resolve a canonical client path to a target handler using exact, ANY-method,
 * and
 * concrete-vs-template matching in that order. */
static int64_t find_route_handler_for_path(cbm_store_t *target_store, const char *path,
                                           const char *method, char *route_qn, size_t route_qn_sz,
                                           char *handler_name, size_t name_sz, char *handler_file,
                                           size_t file_sz) {
    snprintf(route_qn, route_qn_sz, "__route__%s__%s", method && method[0] ? method : "ANY", path);
    int64_t handler_id =
        find_route_handler(target_store, route_qn, handler_name, name_sz, handler_file, file_sz);
    if (handler_id == 0) {
        snprintf(route_qn, route_qn_sz, "__route__ANY__%s", path);
        handler_id = find_route_handler(target_store, route_qn, handler_name, name_sz, handler_file,
                                        file_sz);
    }
    if (handler_id == 0) {
        handler_id = find_route_handler_fuzzy(target_store, path, method, route_qn, route_qn_sz,
                                              handler_name, name_sz, handler_file, file_sz);
    }
    return handler_id;
}

/* Emit CROSS_* edge for a route match: forward into source, reverse into target. */
static void emit_cross_route_bidirectional(cbm_store_t *src_store, const char *src_project,
                                           struct sqlite3 *src_db, int64_t caller_id,
                                           int64_t local_route_id, cbm_store_t *tgt_store,
                                           const char *tgt_project, int64_t handler_id,
                                           const char *route_qn, const char *handler_name,
                                           const char *handler_file, const char *url_path,
                                           const char *method, const char *edge_type) {
    /* Forward: caller → local Route in source DB */
    char fwd[CR_PROPS_BUF];
    build_cross_props(fwd, sizeof(fwd), tgt_project, handler_name, handler_file, url_path,
                      "url_path", method);
    insert_cross_edge(src_store, src_project, caller_id, local_route_id, edge_type, fwd);

    /* Reverse: handler → Route in target DB */
    struct sqlite3 *tgt_db = cbm_store_get_db(tgt_store);
    if (!tgt_db) {
        return;
    }
    sqlite3_stmt *rq = NULL;
    if (sqlite3_prepare_v2(tgt_db, "SELECT id FROM nodes WHERE qualified_name = ?1 LIMIT 1",
                           CBM_NOT_FOUND, &rq, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(rq, SKIP_ONE, route_qn, CBM_NOT_FOUND, SQLITE_STATIC);
    int64_t tgt_route_id = 0;
    if (sqlite3_step(rq) == SQLITE_ROW) {
        tgt_route_id = sqlite3_column_int64(rq, 0);
    }
    sqlite3_finalize(rq);
    if (tgt_route_id == 0) {
        return;
    }

    char caller_name[CBM_SZ_256] = {0};
    char caller_file[CBM_SZ_512] = {0};
    lookup_node_info(src_db, caller_id, caller_name, sizeof(caller_name), caller_file,
                     sizeof(caller_file));

    char rev[CR_PROPS_BUF];
    build_cross_props(rev, sizeof(rev), src_project, caller_name, caller_file, url_path, "url_path",
                      method);
    insert_cross_edge(tgt_store, tgt_project, handler_id, tgt_route_id, edge_type, rev);
}

static int match_http_routes(cbm_store_t *src_store, const char *src_project,
                             cbm_store_t *tgt_store, const char *tgt_project) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    /* Find all HTTP_CALLS edges in source project */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db,
                           "SELECT e.source_id, e.target_id, e.properties FROM edges e "
                           "WHERE e.project = ?1 AND e.type = 'HTTP_CALLS'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < CR_MAX_EDGES) {
        int64_t caller_id = sqlite3_column_int64(s, 0);
        int64_t route_id = sqlite3_column_int64(s, SKIP_ONE);
        const char *props = (const char *)sqlite3_column_text(s, PAIR_LEN);

        char url_path[CBM_SZ_256] = {0};
        char method[CBM_SZ_32] = {0};
        json_str_prop(props, "url_path", url_path, sizeof(url_path));
        json_str_prop(props, "method", method, sizeof(method));
        if (!url_path[0]) {
            continue;
        }

        /* Build the expected Route QN in the target project (authority-stripped
         * and param-canonicalized so client url_path matches the server handler
         * regardless of base URL and framework placeholder syntax). */
        char route_qn[CR_QN_BUF];
        char cpath[CBM_SZ_256];
        const char *curl = cbm_route_canon_path(cr_url_path(url_path), cpath, sizeof(cpath));
        char handler_name[CBM_SZ_256] = {0};
        char handler_file[CBM_SZ_512] = {0};
        int64_t handler_id = find_route_handler_for_path(
            tgt_store, curl, method[0] ? method : NULL, route_qn, sizeof(route_qn), handler_name,
            sizeof(handler_name), handler_file, sizeof(handler_file));
        if (handler_id == 0) {
            /* Some gateway-facing paths intentionally differ from the internal
             *
             * controller prefix. Try the provider alias only after normal
             * matching
             * fails, so exact public routes always win. */
            char alias_path[CBM_SZ_256];
            if (cr_gateway_alias_path(curl, alias_path, sizeof(alias_path))) {
                handler_id = find_route_handler_for_path(
                    tgt_store, alias_path, method[0] ? method : NULL, route_qn, sizeof(route_qn),
                    handler_name, sizeof(handler_name), handler_file, sizeof(handler_file));
            }
        }
        if (handler_id == 0) {
            continue;
        }

        emit_cross_route_bidirectional(src_store, src_project, src_db, caller_id, route_id,
                                       tgt_store, tgt_project, handler_id, route_qn, handler_name,
                                       handler_file, url_path, method, "CROSS_HTTP_CALLS");

        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Phase B: Async matching ─────────────────────────────────────── */

static int match_async_routes(cbm_store_t *src_store, const char *src_project,
                              cbm_store_t *tgt_store, const char *tgt_project) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db,
                           "SELECT e.source_id, e.target_id, e.properties FROM edges e "
                           "WHERE e.project = ?1 AND e.type = 'ASYNC_CALLS'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < CR_MAX_EDGES) {
        int64_t caller_id = sqlite3_column_int64(s, 0);
        int64_t route_id = sqlite3_column_int64(s, SKIP_ONE);
        const char *props = (const char *)sqlite3_column_text(s, PAIR_LEN);

        char url_path[CBM_SZ_256] = {0};
        char broker[CBM_SZ_128] = {0};
        json_str_prop(props, "url_path", url_path, sizeof(url_path));
        json_str_prop(props, "broker", broker, sizeof(broker));
        if (!url_path[0]) {
            continue;
        }

        char route_qn[CR_QN_BUF];
        snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", broker[0] ? broker : "async",
                 url_path);

        char handler_name[CBM_SZ_256] = {0};
        char handler_file[CBM_SZ_512] = {0};
        int64_t handler_id =
            find_route_handler(tgt_store, route_qn, handler_name, sizeof(handler_name),
                               handler_file, sizeof(handler_file));
        if (handler_id == 0) {
            continue;
        }

        char edge_props[CR_PROPS_BUF];
        build_cross_props(edge_props, sizeof(edge_props), tgt_project, handler_name, handler_file,
                          url_path, "url_path", broker);
        insert_cross_edge(src_store, src_project, caller_id, route_id, "CROSS_ASYNC_CALLS",
                          edge_props);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Phase C: Channel matching ───────────────────────────────────── */

/* Try to find a matching listener in target DB for a channel name. */
static bool try_match_channel_listener(cbm_store_t *src_store, const char *src_project,
                                       cbm_store_t *tgt_store, const char *tgt_project,
                                       const char *channel_name, const char *transport,
                                       int64_t emitter_id, int64_t channel_id) {
    struct sqlite3 *tgt_db = cbm_store_get_db(tgt_store);
    if (!tgt_db) {
        return false;
    }
    sqlite3_stmt *tq = NULL;
    if (sqlite3_prepare_v2(tgt_db,
                           "SELECT n.id, e.source_id, fn.name, fn.file_path FROM nodes n "
                           "JOIN edges e ON e.target_id = n.id AND e.type = 'LISTENS_ON' "
                           "JOIN nodes fn ON fn.id = e.source_id "
                           "WHERE n.project = ?1 AND n.name = ?2 AND n.label = 'Channel' LIMIT 1",
                           CBM_NOT_FOUND, &tq, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(tq, SKIP_ONE, tgt_project, CBM_NOT_FOUND, SQLITE_STATIC);
    sqlite3_bind_text(tq, PAIR_LEN, channel_name, CBM_NOT_FOUND, SQLITE_STATIC);

    bool matched = false;
    if (sqlite3_step(tq) == SQLITE_ROW) {
        int64_t tgt_channel_id = sqlite3_column_int64(tq, 0);
        int64_t listener_id = sqlite3_column_int64(tq, SKIP_ONE);
        const char *listener_name = (const char *)sqlite3_column_text(tq, PAIR_LEN);
        const char *listener_file = (const char *)sqlite3_column_text(tq, CR_COL_3);

        /* Forward edge: emitter → local Channel */
        char fwd[CR_PROPS_BUF];
        build_cross_props(fwd, sizeof(fwd), tgt_project, listener_name ? listener_name : "",
                          listener_file ? listener_file : "", channel_name, "channel_name",
                          transport);
        insert_cross_edge(src_store, src_project, emitter_id, channel_id, "CROSS_CHANNEL", fwd);

        /* Reverse edge: listener → target Channel */
        char caller_name[CBM_SZ_256] = {0};
        char caller_file[CBM_SZ_512] = {0};
        lookup_node_info(cbm_store_get_db(src_store), emitter_id, caller_name, sizeof(caller_name),
                         caller_file, sizeof(caller_file));

        char rev[CR_PROPS_BUF];
        build_cross_props(rev, sizeof(rev), src_project, caller_name, caller_file, channel_name,
                          "channel_name", transport);
        insert_cross_edge(tgt_store, tgt_project, listener_id, tgt_channel_id, "CROSS_CHANNEL",
                          rev);
        matched = true;
    }
    sqlite3_finalize(tq);
    return matched;
}

static int match_channels(cbm_store_t *src_store, const char *src_project, cbm_store_t *tgt_store,
                          const char *tgt_project) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db,
                           "SELECT DISTINCT n.id, n.name, n.qualified_name, n.properties, "
                           "e.source_id FROM nodes n "
                           "JOIN edges e ON e.target_id = n.id AND e.type = 'EMITS' "
                           "WHERE n.project = ?1 AND n.label = 'Channel'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < CR_MAX_EDGES) {
        const char *channel_name = (const char *)sqlite3_column_text(s, SKIP_ONE);
        const char *channel_qn = (const char *)sqlite3_column_text(s, PAIR_LEN);
        if (!channel_name || !channel_qn) {
            continue;
        }
        int64_t channel_id = sqlite3_column_int64(s, 0);
        const char *channel_props = (const char *)sqlite3_column_text(s, CR_COL_3);
        int64_t emitter_id = sqlite3_column_int64(s, CR_COL_4);

        char transport[CBM_SZ_64] = {0};
        json_str_prop(channel_props, "transport", transport, sizeof(transport));

        if (try_match_channel_listener(src_store, src_project, tgt_store, tgt_project, channel_name,
                                       transport, emitter_id, channel_id)) {
            count++;
        }
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Phase D: Generic route-type matcher (gRPC, GraphQL, tRPC) ──── */

/* Look up a node's qualified_name by id. Returns true if found. */
static bool lookup_node_qn(struct sqlite3 *db, int64_t node_id, char *out, size_t out_sz) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT qualified_name FROM nodes WHERE id = ?1", CBM_NOT_FOUND, &st,
                           NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, SKIP_ONE, node_id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(st, 0);
        if (qn) {
            snprintf(out, out_sz, "%s", qn);
            found = true;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* Match edges of a given type against Route nodes with a given QN prefix.
 * Reuses the same infrastructure as HTTP/async matching. */
static int match_typed_routes(cbm_store_t *src_store, const char *src_project,
                              cbm_store_t *tgt_store, const char *tgt_project,
                              const char *edge_type, const char *svc_key, const char *method_key,
                              const char *cross_edge_type) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    char sql[CBM_SZ_256];
    snprintf(sql, sizeof(sql),
             "SELECT e.source_id, e.target_id, e.properties FROM edges e "
             "WHERE e.project = ?1 AND e.type = '%s'",
             edge_type);

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db, sql, CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < CR_MAX_EDGES) {
        int64_t caller_id = sqlite3_column_int64(s, 0);
        int64_t route_id = sqlite3_column_int64(s, SKIP_ONE);
        const char *props = (const char *)sqlite3_column_text(s, PAIR_LEN);

        char svc_val[CBM_SZ_256] = {0};
        char meth_val[CBM_SZ_256] = {0};
        json_str_prop(props, svc_key, svc_val, sizeof(svc_val));
        json_str_prop(props, method_key, meth_val, sizeof(meth_val));
        if (!svc_val[0] && !meth_val[0]) {
            continue;
        }

        /* Look up the Route QN from the target node (already points to the Route). */
        char route_qn[CR_QN_BUF] = {0};
        if (!lookup_node_qn(src_db, route_id, route_qn, sizeof(route_qn))) {
            continue;
        }

        char handler_name[CBM_SZ_256] = {0};
        char handler_file[CBM_SZ_512] = {0};
        int64_t handler_id =
            find_route_handler(tgt_store, route_qn, handler_name, sizeof(handler_name),
                               handler_file, sizeof(handler_file));
        if (handler_id == 0) {
            continue;
        }

        emit_cross_route_bidirectional(src_store, src_project, src_db, caller_id, route_id,
                                       tgt_store, tgt_project, handler_id, route_qn, handler_name,
                                       handler_file, svc_val, svc_key, cross_edge_type);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Collect target projects ─────────────────────────────────────── */

/* When target_projects = ["*"], scan the cache directory for all .db files. */
static int collect_all_projects(char ***out) {
    const char *dir = cr_cache_dir();
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        *out = NULL;
        return 0;
    }

    int cap = CR_INIT_CAP;
    int count = 0;
    char **projects = malloc((size_t)cap * sizeof(char *));
    if (!projects) {
        cbm_closedir(d);
        *out = NULL;
        return 0;
    }

    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        size_t len = strlen(ent->name);
        if (len < CR_COL_4 || strcmp(ent->name + len - CR_DB_EXT_LEN, ".db") != 0) {
            continue;
        }
        if (strstr(ent->name, "_cross_repo") || strstr(ent->name, "_config")) {
            continue;
        }
        if (strstr(ent->name, "-wal") || strstr(ent->name, "-shm")) {
            continue;
        }
        if (count >= cap) {
            cap *= PAIR_LEN;
            char **tmp = realloc(projects, (size_t)cap * sizeof(char *));
            if (!tmp) {
                break;
            }
            projects = tmp;
        }
        /* Strip .db extension */
        size_t project_len = len - CR_DB_EXT_LEN;
        char project_name[CR_PATH_BUF];
        if (project_len == 0 || project_len >= sizeof(project_name)) {
            continue;
        }
        memcpy(project_name, ent->name, project_len);
        project_name[project_len] = '\0';
        if (!cbm_validate_project_name(project_name)) {
            continue;
        }
        projects[count] = malloc(project_len + 1U);
        if (!projects[count]) {
            break;
        }
        memcpy(projects[count], project_name, project_len + 1U);
        count++;
    }
    cbm_closedir(d);

    *out = projects;
    return count;
}

static void free_project_list(char **projects, int count) {
    for (int i = 0; i < count; i++) {
        free(projects[i]);
    }
    free(projects);
}

/* ── Entry point ─────────────────────────────────────────────────── */

cbm_cross_repo_result_t cbm_cross_repo_match(const char *project, const char **target_projects,
                                             int target_count) {
    cbm_cross_repo_result_t result = {0};
    if (!cbm_validate_project_name(project) || target_count < 0 || target_count > CR_TARGET_MAX ||
        (target_count > 0 && !target_projects)) {
        return result;
    }
    for (int i = 0; i < target_count; i++) {
        if (!target_projects[i] || !target_projects[i][0] ||
            (strcmp(target_projects[i], "*") != 0 &&
             !cbm_validate_project_name(target_projects[i]))) {
            return result;
        }
        if (strcmp(target_projects[i], "*") == 0 && target_count != 1) {
            return result;
        }
    }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Open source project store (read-write) */
    char src_path[CR_PATH_BUF];
    cr_db_path(project, src_path, sizeof(src_path));
    cbm_store_t *src_store = cbm_store_open_path(src_path);
    if (!src_store) {
        return result;
    }

    /* Clean existing CROSS_* edges for this project */
    delete_cross_edges(src_store, project);
    clear_reverse_edges_for_source(project);
    cr_package_list_t src_packages = {0};
    cbm_project_t src_info = {0};
    if (cbm_store_get_project(src_store, project, &src_info) == CBM_STORE_OK &&
        src_info.root_path) {
        cr_scan_packages(src_info.root_path, "", 0, &src_packages);
    }

    /* Resolve target projects */
    char **resolved = NULL;
    int resolved_count = 0;
    bool own_list = false;

    if (target_count == SKIP_ONE && strcmp(target_projects[0], "*") == 0) {
        resolved_count = collect_all_projects(&resolved);
        own_list = true;
    } else {
        resolved = (char **)target_projects;
        resolved_count = target_count;
    }

    /* Match against each target */
    for (int i = 0; i < resolved_count; i++) {
        const char *tgt = resolved[i];
        if (strcmp(tgt, project) == 0) {
            continue; /* skip self */
        }

        char tgt_path[CR_PATH_BUF];
        cr_db_path(tgt, tgt_path, sizeof(tgt_path));

        /* Open target store read-write (for bidirectional edge writes) */
        cbm_store_t *tgt_store = cbm_store_open_path(tgt_path);
        if (!tgt_store) {
            continue;
        }

        cr_package_list_t tgt_packages = {0};
        cbm_project_t tgt_info = {0};
        if (cbm_store_get_project(tgt_store, tgt, &tgt_info) == CBM_STORE_OK &&
            tgt_info.root_path) {
            cr_scan_packages(tgt_info.root_path, "", 0, &tgt_packages);
        }
        int package_matches = cr_match_package_imports(src_store, project, tgt_store, tgt,
                                                       &src_packages, &tgt_packages);
        /* Run the reverse direction as well. Imports live in the consumer DB,
         * so invoking the matcher from the provider side must still recreate
         * both graph-visible directions. */
        int reverse_package_matches = cr_match_package_imports(tgt_store, tgt, src_store, project,
                                                               &tgt_packages, &src_packages);
        int manifest_matches =
            src_info.root_path
                ? cr_match_manifest_dependencies(src_store, project, src_info.root_path, tgt_store,
                                                 tgt, &src_packages, &tgt_packages)
                : 0;
        result.package_import_edges += package_matches + reverse_package_matches + manifest_matches;
        result.project_dependency_edges +=
            package_matches + reverse_package_matches + manifest_matches;
        if (tgt_info.root_path || tgt_info.name || tgt_info.indexed_at) {
            cbm_project_free_fields(&tgt_info);
        }

        result.http_edges += match_http_routes(src_store, project, tgt_store, tgt);
        /* Reverse direction: when this pass runs from the provider side, the
         * consumer's HTTP_CALLS live in tgt, not src — the forward pass above
         * finds nothing because the provider has no outbound calls. This also
         * re-creates the provider-side reverse edges that delete_cross_edges
         * just wiped, which a provider-side run previously destroyed for good.
         * A caller edge lives in exactly one DB, so the two directions scan
         * disjoint edge sets and never double-count a pair; the store's
         * (source, target, type) upsert keeps re-recorded rows unique. (#523) */
        result.http_edges += match_http_routes(tgt_store, tgt, src_store, project);
        result.async_edges += match_async_routes(src_store, project, tgt_store, tgt);
        result.channel_edges += match_channels(src_store, project, tgt_store, tgt);
        result.grpc_edges += match_typed_routes(src_store, project, tgt_store, tgt, "GRPC_CALLS",
                                                "service", "method", "CROSS_GRPC_CALLS");
        result.graphql_edges +=
            match_typed_routes(src_store, project, tgt_store, tgt, "GRAPHQL_CALLS", "operation",
                               "operation", "CROSS_GRAPHQL_CALLS");
        result.trpc_edges += match_typed_routes(src_store, project, tgt_store, tgt, "TRPC_CALLS",
                                                "procedure", "procedure", "CROSS_TRPC_CALLS");
        result.projects_scanned++;

        cbm_store_close(tgt_store);
    }

    cbm_store_close(src_store);
    if (src_info.root_path || src_info.name || src_info.indexed_at) {
        cbm_project_free_fields(&src_info);
    }

    if (own_list) {
        free_project_list(resolved, resolved_count);
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    result.elapsed_ms = ((double)(t1.tv_sec - t0.tv_sec) * CR_MS_PER_SEC) +
                        ((double)(t1.tv_nsec - t0.tv_nsec) / CR_NS_PER_MS);

    int total = result.http_edges + result.async_edges + result.channel_edges + result.grpc_edges +
                result.graphql_edges + result.trpc_edges + result.package_import_edges +
                result.project_dependency_edges;
    cbm_log_info("cross_repo.done", "project", project, "total", cr_itoa(total));

    return result;
}
