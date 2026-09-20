#include "document_refs.h"
#include "yyjson/yyjson.h"

#include <stdlib.h>
#include <string.h>

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
