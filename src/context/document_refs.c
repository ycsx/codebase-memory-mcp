#include "document_refs.h"
#include "yyjson/yyjson.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum { DOCUMENT_REFS_MAX = 100 };

typedef struct {
    cbm_node_t source;
    cbm_node_t target;
    char *properties;
    const char *document_qn;
    int64_t line;
    int64_t edge_id;
} document_ref_t;

static const char *text(const char *s) {
    return s ? s : "";
}

static void ref_clear(document_ref_t *ref) {
    cbm_node_free_fields(&ref->source);
    cbm_node_free_fields(&ref->target);
    free(ref->properties);
}

static int compare_ids(const void *lhs, const void *rhs) {
    int64_t a = *(const int64_t *)lhs;
    int64_t b = *(const int64_t *)rhs;
    return (a > b) - (a < b);
}

static int compare_edges(const void *lhs, const void *rhs) {
    const cbm_edge_t *a = lhs;
    const cbm_edge_t *b = rhs;
    int order = (a->source_id > b->source_id) - (a->source_id < b->source_id);
    return order ? order : (a->id > b->id) - (a->id < b->id);
}

static int compare_refs(const void *lhs, const void *rhs) {
    const document_ref_t *a = lhs;
    const document_ref_t *b = rhs;
    int order = strcmp(text(a->source.file_path), text(b->source.file_path));
    if (!order) {
        order = (a->line > b->line) - (a->line < b->line);
    }
    if (!order) {
        order = strcmp(text(a->target.qualified_name), text(b->target.qualified_name));
    }
    if (!order) {
        order = strcmp(text(a->source.qualified_name), text(b->source.qualified_name));
    }
    return order ? order : (a->edge_id > b->edge_id) - (a->edge_id < b->edge_id);
}

static yyjson_mut_val *node_json(yyjson_mut_doc *doc, const cbm_node_t *node) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (!obj || !yyjson_mut_obj_add_int(doc, obj, "id", node->id) ||
        !yyjson_mut_obj_add_strcpy(doc, obj, "label", text(node->label)) ||
        !yyjson_mut_obj_add_strcpy(doc, obj, "name", text(node->name)) ||
        !yyjson_mut_obj_add_strcpy(doc, obj, "qualified_name", text(node->qualified_name)) ||
        !yyjson_mut_obj_add_strcpy(doc, obj, "file_path", text(node->file_path)) ||
        !yyjson_mut_obj_add_int(doc, obj, "start_line", node->start_line) ||
        !yyjson_mut_obj_add_int(doc, obj, "end_line", node->end_line)) {
        return NULL;
    }
    return obj;
}

char *cbm_document_refs_json(cbm_store_t *store, const char *project, const cbm_node_t *targets,
                             int target_count, int limit) {
    if (!store || !project || !project[0] || target_count < 0 || (target_count > 0 && !targets)) {
        return NULL;
    }
    limit = limit < 0 ? 0 : limit > DOCUMENT_REFS_MAX ? DOCUMENT_REFS_MAX : limit;
    document_ref_t refs[DOCUMENT_REFS_MAX] = {0};
    int kept = 0;
    int64_t total = 0;
    int unknown = 0;
    int limited = 0;
    bool invalid_evidence = false;
    cbm_node_t *documents = NULL;
    int document_count = 0;
    int64_t *ids = NULL;
    yyjson_mut_doc *json = NULL;
    char *result = NULL;
    if (cbm_store_find_nodes_by_label(store, project, "Document", &documents, &document_count) !=
        CBM_STORE_OK) {
        goto cleanup;
    }
    for (int i = 0; i < document_count; i++) {
        const char *raw = text(documents[i].properties_json);
        yyjson_doc *props = yyjson_read(raw, strlen(raw), 0);
        yyjson_val *analysis = yyjson_obj_get(yyjson_doc_get_root(props), "document_links");
        const char *status = yyjson_get_str(yyjson_obj_get(analysis, "status"));
        if (yyjson_get_int(yyjson_obj_get(analysis, "index_version")) != 1 || !status ||
            (strcmp(status, "ok") && strcmp(status, "limited"))) {
            unknown++;
        } else if (!strcmp(status, "limited")) {
            limited++;
        }
        yyjson_doc_free(props);
    }
    if (target_count > 0) {
        ids = malloc((size_t)target_count * sizeof(*ids));
        if (!ids) {
            goto cleanup;
        }
        for (int i = 0; i < target_count; i++) {
            ids[i] = targets[i].id;
        }
        qsort(ids, (size_t)target_count, sizeof(*ids), compare_ids);
    }
    for (int t = 0; t < target_count; t++) {
        if (ids[t] <= 0 || (t > 0 && ids[t] == ids[t - 1])) {
            continue;
        }
        cbm_node_t target = {0};
        int rc = cbm_store_find_node_by_id(store, ids[t], &target);
        if (rc == CBM_STORE_NOT_FOUND) {
            continue;
        }
        if (rc != CBM_STORE_OK) {
            goto cleanup;
        }
        bool same_project = !strcmp(text(target.project), project);
        cbm_node_free_fields(&target);
        if (!same_project) {
            continue;
        }
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (cbm_store_find_edges_by_target_type(store, ids[t], "REFERENCES", &edges, &edge_count) !=
            CBM_STORE_OK) {
            cbm_store_free_edges(edges, edge_count);
            goto cleanup;
        }
        bool failed = false;
        int64_t previous_source = 0;
        if (edge_count > 1) {
            qsort(edges, (size_t)edge_count, sizeof(*edges), compare_edges);
        }
        for (int e = 0; e < edge_count && !failed; e++) {
            if (strcmp(text(edges[e].project), project) || edges[e].source_id == previous_source) {
                continue;
            }
            const char *raw = text(edges[e].properties_json);
            yyjson_doc *props = yyjson_read(raw, strlen(raw), 0);
            yyjson_val *properties = yyjson_doc_get_root(props);
            const char *producer = yyjson_get_str(yyjson_obj_get(properties, "producer"));
            if (!producer || strcmp(producer, "document_links")) {
                yyjson_doc_free(props);
                continue;
            }
            document_ref_t ref = {.edge_id = edges[e].id};
            ref.line = yyjson_get_sint(
                yyjson_obj_get(yyjson_obj_get(properties, "document_span"), "start_line"));
            yyjson_doc_free(props);
            rc = cbm_store_find_node_by_id(store, edges[e].source_id, &ref.source);
            if (rc != CBM_STORE_OK) {
                failed = true;
                ref_clear(&ref);
                continue;
            }
            if (strcmp(text(ref.source.project), project) ||
                (strcmp(text(ref.source.label), "Document") &&
                 strcmp(text(ref.source.label), "Section"))) {
                ref_clear(&ref);
                continue;
            }
            for (int d = 0; d < document_count; d++) {
                if (!strcmp(text(documents[d].file_path), text(ref.source.file_path))) {
                    ref.document_qn = documents[d].qualified_name;
                    break;
                }
            }
            if (!ref.document_qn || ref.line < 1) {
                invalid_evidence = true;
            }
            previous_source = edges[e].source_id;
            total++;
            if (!limit) {
                ref_clear(&ref);
                continue;
            }
            if (cbm_store_find_node_by_id(store, ids[t], &ref.target) != CBM_STORE_OK ||
                !(ref.properties = strdup(raw))) {
                failed = true;
                ref_clear(&ref);
                continue;
            }
            if (kept == limit) {
                if (compare_refs(&ref, &refs[kept - 1]) >= 0) {
                    ref_clear(&ref);
                    continue;
                }
                ref_clear(&refs[--kept]);
            }
            refs[kept++] = ref;
            qsort(refs, (size_t)kept, sizeof(*refs), compare_refs);
        }
        cbm_store_free_edges(edges, edge_count);
        if (failed) {
            goto cleanup;
        }
    }
    json = yyjson_mut_doc_new(NULL);
    if (!json) {
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(json);
    yyjson_mut_val *array = yyjson_mut_arr(json);
    const char *status = limited || invalid_evidence  ? "limited"
                         : unknown || !document_count ? "unknown"
                                                      : "ok";
    const char *reason = limited            ? "document_analysis_limited"
                         : invalid_evidence ? "incomplete_reference_evidence"
                         : unknown          ? "reindex_required"
                         : !document_count  ? "no_indexed_documents"
                                            : "supported_subset";
    if (!root || !array || !yyjson_mut_obj_add_val(json, root, "references", array) ||
        !yyjson_mut_obj_add_int(json, root, "total", total) ||
        !yyjson_mut_obj_add_int(json, root, "returned", kept) ||
        !yyjson_mut_obj_add_bool(json, root, "truncated", total > kept) ||
        !yyjson_mut_obj_add_str(json, root, "status", status) ||
        !yyjson_mut_obj_add_str(json, root, "reason", reason) ||
        !yyjson_mut_obj_add_str(json, root, "scope", "single_line_links_and_exact_inline_code") ||
        !yyjson_mut_obj_add_int(json, root, "documents_unknown", unknown) ||
        !yyjson_mut_obj_add_int(json, root, "documents_limited", limited) ||
        !yyjson_mut_obj_add_int(json, root, "documents_total", document_count)) {
        goto cleanup;
    }
    yyjson_mut_doc_set_root(json, root);
    for (int i = 0; i < kept; i++) {
        document_ref_t *ref = &refs[i];
        yyjson_mut_val *item = yyjson_mut_obj(json);
        yyjson_mut_val *source = node_json(json, &ref->source);
        yyjson_mut_val *target = node_json(json, &ref->target);
        yyjson_doc *props = yyjson_read(ref->properties, strlen(ref->properties), 0);
        yyjson_mut_val *evidence = yyjson_val_mut_copy(json, yyjson_doc_get_root(props));
        yyjson_doc_free(props);
        if (!item || !source || !target || !evidence ||
            !yyjson_mut_obj_add_val(json, item, "source", source) ||
            !yyjson_mut_obj_add_val(json, item, "target", target) ||
            !yyjson_mut_obj_add_val(json, item, "properties", evidence) ||
            !yyjson_mut_obj_add_strcpy(json, item, "document_qualified_name",
                                       text(ref->document_qn)) ||
            !yyjson_mut_arr_add_val(array, item)) {
            goto cleanup;
        }
    }
    result = yyjson_mut_write(json, 0, NULL);
cleanup:
    yyjson_mut_doc_free(json);
    for (int i = 0; i < kept; i++) {
        ref_clear(&refs[i]);
    }
    cbm_store_free_nodes(documents, document_count);
    free(ids);
    return result;
}

typedef struct {
    cbm_node_t *node;
    int64_t count;
} coverage_file_t;

static int coverage_compare_files(const void *lhs, const void *rhs) {
    const coverage_file_t *a = lhs;
    const coverage_file_t *b = rhs;
    int order = strcmp(text(a->node->file_path), text(b->node->file_path));
    return order ? order : strcmp(text(a->node->qualified_name), text(b->node->qualified_name));
}

static int coverage_find_file(coverage_file_t *files, int count, const char *path) {
    int lo = 0;
    int hi = count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int order = strcmp(text(files[mid].node->file_path), text(path));
        if (order < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < count && !strcmp(text(files[lo].node->file_path), text(path)) ? lo : -1;
}

static bool coverage_markdown(const char *path) {
    const char *ext = strrchr(text(path), '.');
    if (!ext || strlen(ext) >= 16) {
        return false;
    }
    char lower[16];
    size_t i = 0;
    for (; ext[i]; i++) {
        lower[i] = (char)tolower((unsigned char)ext[i]);
    }
    lower[i] = '\0';
    return !strcmp(lower, ".md") || !strcmp(lower, ".mdx") || !strcmp(lower, ".markdown");
}

static const char *coverage_document_status(yyjson_val *analysis) {
    const char *status = yyjson_get_str(yyjson_obj_get(analysis, "status"));
    if (yyjson_get_int(yyjson_obj_get(analysis, "index_version")) != 1 || !status ||
        (strcmp(status, "ok") && strcmp(status, "limited"))) {
        return "unknown";
    }
    return status;
}

char *cbm_document_coverage_json(cbm_store_t *store, const char *project, const char *view,
                                const char *status, int offset, int limit) {
    if (!store || !project || !project[0] || !view || offset < 0 ||
        (strcmp(view, "code") && strcmp(view, "documents"))) {
        return NULL;
    }
    bool code_view = !strcmp(view, "code");
    if (status && status[0] &&
        (code_view ? strcmp(status, "referenced") && strcmp(status, "not_referenced")
                   : strcmp(status, "ok") && strcmp(status, "limited") &&
                         strcmp(status, "unknown"))) {
        return NULL;
    }
    limit = limit < 0 ? 0 : limit > DOCUMENT_REFS_MAX ? DOCUMENT_REFS_MAX : limit;
    cbm_node_t *nodes = NULL;
    cbm_node_t *documents = NULL;
    cbm_edge_t *edges = NULL;
    coverage_file_t *files = NULL;
    coverage_file_t *docs = NULL;
    int node_count = 0, document_count = 0, edge_count = 0, file_count = 0;
    int referenced = 0, limited = 0, unknown = 0;
    yyjson_mut_doc *json = NULL;
    char *result = NULL;
    if (cbm_store_find_nodes_by_label(store, project, "File", &nodes, &node_count) != CBM_STORE_OK ||
        cbm_store_find_nodes_by_label(store, project, "Document", &documents, &document_count) !=
            CBM_STORE_OK ||
        cbm_store_find_edges_by_type(store, project, "REFERENCES", &edges, &edge_count) !=
            CBM_STORE_OK) {
        goto coverage_cleanup;
    }
    files = calloc((size_t)node_count + 1, sizeof(*files));
    docs = calloc((size_t)document_count + 1, sizeof(*docs));
    if (!files || !docs) {
        goto coverage_cleanup;
    }
    for (int i = 0; i < document_count; i++) {
        docs[i].node = &documents[i];
        const char *raw = text(documents[i].properties_json);
        yyjson_doc *props = yyjson_read(raw, strlen(raw), 0);
        const char *state = coverage_document_status(
            yyjson_obj_get(yyjson_doc_get_root(props), "document_links"));
        limited += !strcmp(state, "limited");
        unknown += !strcmp(state, "unknown");
        yyjson_doc_free(props);
    }
    qsort(docs, (size_t)document_count, sizeof(*docs), coverage_compare_files);
    for (int i = 0; i < node_count; i++) {
        if (!text(nodes[i].file_path)[0] || coverage_markdown(nodes[i].file_path) ||
            coverage_find_file(docs, document_count, nodes[i].file_path) >= 0) {
            continue;
        }
        files[file_count++].node = &nodes[i];
    }
    qsort(files, (size_t)file_count, sizeof(*files), coverage_compare_files);
    int unique = 0;
    for (int i = 0; i < file_count; i++) {
        if (!unique || strcmp(files[i].node->file_path, files[unique - 1].node->file_path)) {
            files[unique++] = files[i];
        }
    }
    file_count = unique;
    /* Scan project references once, then map symbol targets to sorted file paths. */
    for (int i = 0; i < edge_count; i++) {
        const char *raw = text(edges[i].properties_json);
        yyjson_doc *props = yyjson_read(raw, strlen(raw), 0);
        const char *producer =
            yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(props), "producer"));
        bool eligible = producer && !strcmp(producer, "document_links") &&
                        !strcmp(text(edges[i].project), project);
        yyjson_doc_free(props);
        if (!eligible) {
            continue;
        }
        cbm_node_t source = {0}, target = {0};
        int source_rc = cbm_store_find_node_by_id(store, edges[i].source_id, &source);
        int target_rc = cbm_store_find_node_by_id(store, edges[i].target_id, &target);
        if ((source_rc != CBM_STORE_OK && source_rc != CBM_STORE_NOT_FOUND) ||
            (target_rc != CBM_STORE_OK && target_rc != CBM_STORE_NOT_FOUND)) {
            cbm_node_free_fields(&source);
            cbm_node_free_fields(&target);
            goto coverage_cleanup;
        }
        if (source_rc == CBM_STORE_OK && target_rc == CBM_STORE_OK &&
            !strcmp(text(source.project), project) && !strcmp(text(target.project), project) &&
            (!strcmp(text(source.label), "Document") || !strcmp(text(source.label), "Section"))) {
            int index = coverage_find_file(files, file_count, target.file_path);
            if (index >= 0) {
                files[index].count++;
            }
        }
        cbm_node_free_fields(&source);
        cbm_node_free_fields(&target);
    }
    for (int i = 0; i < file_count; i++) {
        referenced += files[i].count > 0;
    }
    json = yyjson_mut_doc_new(NULL);
    if (!json) {
        goto coverage_cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(json);
    yyjson_mut_val *summary = yyjson_mut_obj(json);
    yyjson_mut_val *items = yyjson_mut_arr(json);
    if (!root || !summary || !items) {
        goto coverage_cleanup;
    }
    yyjson_mut_doc_set_root(json, root);
    int total = 0, returned = 0;
    for (int i = 0; i < (code_view ? file_count : document_count); i++) {
        cbm_node_t *node = code_view ? files[i].node : docs[i].node;
        const char *raw = text(node->properties_json);
        yyjson_doc *props = code_view ? NULL : yyjson_read(raw, strlen(raw), 0);
        yyjson_val *analysis = yyjson_obj_get(yyjson_doc_get_root(props), "document_links");
        const char *state = code_view ? (files[i].count ? "referenced" : "not_referenced")
                                     : coverage_document_status(analysis);
        if (status && status[0] && strcmp(status, state)) {
            yyjson_doc_free(props);
            continue;
        }
        if (total++ < offset || returned >= limit) {
            yyjson_doc_free(props);
            continue;
        }
        yyjson_mut_val *item = yyjson_mut_obj(json);
        bool ok = item && yyjson_mut_obj_add_strcpy(json, item, "file_path", text(node->file_path)) &&
                  yyjson_mut_obj_add_strcpy(json, item, "status", state);
        if (code_view) {
            ok = ok && yyjson_mut_obj_add_int(json, item, "reference_count", files[i].count);
        } else {
            yyjson_mut_val *reasons = yyjson_mut_arr(json);
            const char *reason = !strcmp(state, "unknown") ? "reindex_required"
                : yyjson_get_str(yyjson_obj_get(analysis, "reason"));
            ok = ok && reasons &&
                 yyjson_mut_obj_add_strcpy(json, item, "qualified_name", text(node->qualified_name)) &&
                 yyjson_mut_obj_add_val(json, item, "reasons", reasons) &&
                 (!reason || yyjson_mut_arr_add_strcpy(json, reasons, reason));
        }
        yyjson_doc_free(props);
        if (!ok || !yyjson_mut_arr_add_val(items, item)) {
            goto coverage_cleanup;
        }
        returned++;
    }
    bool has_more = offset < total && returned < total - offset;
    if (!yyjson_mut_obj_add_strcpy(json, root, "project", project) ||
        !yyjson_mut_obj_add_strcpy(json, root, "view", view) ||
        !yyjson_mut_obj_add_int(json, root, "total", total) ||
        !yyjson_mut_obj_add_int(json, root, "offset", offset) ||
        !yyjson_mut_obj_add_int(json, root, "returned", returned) ||
        !yyjson_mut_obj_add_bool(json, root, "has_more", has_more) ||
        !(has_more ? yyjson_mut_obj_add_int(json, root, "next_offset", (int64_t)offset + returned)
                   : yyjson_mut_obj_add_null(json, root, "next_offset")) ||
        !yyjson_mut_obj_add_val(json, root, "summary", summary) ||
        !yyjson_mut_obj_add_val(json, root, "items", items) ||
        !yyjson_mut_obj_add_int(json, summary, "indexed_code_files", file_count) ||
        !yyjson_mut_obj_add_int(json, summary, "referenced_code_files", referenced) ||
        !yyjson_mut_obj_add_int(json, summary, "indexed_documents", document_count) ||
        !yyjson_mut_obj_add_int(json, summary, "limited_documents", limited) ||
        !yyjson_mut_obj_add_int(json, summary, "unknown_documents", unknown) ||
        !yyjson_mut_obj_add_str(json, root, "limitation",
            "Current indexed File nodes and document_links REFERENCES only; reference_count counts "
            "edges, including symbol targets in each file. Unreferenced files are not proof of "
            "missing documentation. Parser status describes only the supported Markdown subset.")) {
        goto coverage_cleanup;
    }
    result = yyjson_mut_write(json, 0, NULL);
coverage_cleanup:
    yyjson_mut_doc_free(json);
    free(files);
    free(docs);
    cbm_store_free_nodes(nodes, node_count);
    cbm_store_free_nodes(documents, document_count);
    cbm_store_free_edges(edges, edge_count);
    return result;
}
