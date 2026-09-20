#include "test_framework.h"
#include "context/document_refs.h"
#include "yyjson/yyjson.h"

static int64_t refs_node(cbm_store_t *store, const char *project, const char *label, const char *qn,
                         const char *file, const char *props) {
    if (cbm_store_upsert_project(store, project, "/fixture") != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    cbm_node_t node = {.project = project,
                       .label = label,
                       .name = qn,
                       .qualified_name = qn,
                       .file_path = file,
                       .start_line = 1,
                       .end_line = 20,
                       .properties_json = props};
    return cbm_store_upsert_node(store, &node);
}

static void refs_edge(cbm_store_t *store, const char *project, int64_t source, int64_t target,
                      int line, const char *producer) {
    char props[512];
    snprintf(props, sizeof(props),
             "{\"producer\":\"%s\",\"source\":\"symbol_match\",\"confidence\":1,"
             "\"document_span\":{\"start_line\":%d,\"end_line\":%d},"
             "\"matched_text\":\"demo.core.compute\",\"index_version\":1}",
             producer, line, line);
    cbm_edge_t edge = {.project = project,
                       .source_id = source,
                       .target_id = target,
                       .type = "REFERENCES",
                       .properties_json = props};
    cbm_store_insert_edge(store, &edge);
}

static const char *refs_ok = "{\"document_links\":{\"status\":\"ok\",\"index_version\":1}}";

TEST(document_refs_order_evidence_and_dedup) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    int64_t target = refs_node(store, "demo", "Function", "demo.compute", "src/core.c", "{}");
    int64_t doc = refs_node(store, "demo", "Document", "demo.doc", "docs/a.md", refs_ok);
    int64_t section = refs_node(store, "demo", "Section", "demo.doc.usage", "docs/a.md", "{}");
    int64_t later = refs_node(store, "demo", "Document", "demo.zdoc", "docs/z.md", refs_ok);
    refs_edge(store, "demo", later, target, 1, "document_links");
    refs_edge(store, "demo", doc, target, 8, "document_links");
    refs_edge(store, "demo", section, target, 3, "document_links");
    cbm_edge_t duplicate = {.project = "demo",
                            .source_id = section,
                            .target_id = target,
                            .type = "REFERENCES",
                            .properties_json =
                                "{\"producer\":\"document_links\",\"local_name\":\"alias\","
                                "\"document_span\":{\"start_line\":3,\"end_line\":3}}"};
    ASSERT_TRUE(cbm_store_insert_edge(store, &duplicate) > 0);
    cbm_node_t targets[] = {{.id = target}, {.id = target}};
    char *raw = cbm_document_refs_json(store, "demo", targets, 2, 2);
    ASSERT_NOT_NULL(raw);
    yyjson_doc *json = yyjson_read(raw, strlen(raw), 0);
    ASSERT_NOT_NULL(json);
    yyjson_val *root = yyjson_doc_get_root(json);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "total")), 3);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "returned")), 2);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(root, "truncated")));
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "status")), "ok");
    yyjson_val *first = yyjson_arr_get(yyjson_obj_get(root, "references"), 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(first, "document_qualified_name")), "demo.doc");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(first, "source"), "qualified_name")),
                  "demo.doc.usage");
    yyjson_val *props = yyjson_obj_get(first, "properties");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(props, "matched_text")), "demo.core.compute");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(yyjson_obj_get(props, "document_span"), "start_line")),
              3);
    yyjson_doc_free(json);
    free(raw);
    raw = cbm_document_refs_json(store, "demo", targets, 2, 0);
    ASSERT_NOT_NULL(raw);
    json = yyjson_read(raw, strlen(raw), 0);
    root = yyjson_doc_get_root(json);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "total")), 3);
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(root, "references")), 0);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(root, "truncated")));
    yyjson_doc_free(json);
    free(raw);
    cbm_store_close(store);
    PASS();
}

TEST(document_refs_project_and_producer_isolation) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    int64_t target = refs_node(store, "demo", "Function", "demo.compute", "src/core.c", "{}");
    int64_t foreign = refs_node(store, "other", "Function", "other.compute", "src/core.c", "{}");
    int64_t doc = refs_node(store, "demo", "Document", "demo.doc", "docs/a.md", refs_ok);
    int64_t other = refs_node(store, "other", "Document", "other.doc", "docs/b.md", refs_ok);
    int64_t manual = refs_node(store, "demo", "Document", "demo.manual", "docs/m.md", refs_ok);
    int64_t foreign_edge =
        refs_node(store, "demo", "Document", "demo.foreign_edge", "docs/f.md", refs_ok);
    int64_t code = refs_node(store, "demo", "Function", "demo.caller", "src/caller.c", "{}");
    refs_edge(store, "demo", doc, target, 2, "document_links");
    refs_edge(store, "demo", manual, target, 2, "manual");
    refs_edge(store, "other", other, foreign, 2, "document_links");
    refs_edge(store, "demo", other, target, 2, "document_links");
    refs_edge(store, "demo", code, target, 2, "document_links");
    refs_edge(store, "other", foreign_edge, target, 2, "document_links");
    cbm_node_t targets[] = {{.id = target}, {.id = foreign}, {.id = 999999}};
    char *raw = cbm_document_refs_json(store, "demo", targets, 3, 100);
    ASSERT_NOT_NULL(raw);
    yyjson_doc *json = yyjson_read(raw, strlen(raw), 0);
    ASSERT_NOT_NULL(json);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(yyjson_doc_get_root(json), "total")), 1);
    yyjson_doc_free(json);
    free(raw);
    cbm_store_close(store);
    PASS();
}

TEST(document_refs_analysis_is_not_negative_proof) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    char *raw = cbm_document_refs_json(store, "demo", NULL, 0, 10);
    ASSERT_NOT_NULL(raw);
    yyjson_doc *json = yyjson_read(raw, strlen(raw), 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(json), "status")), "unknown");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(json), "reason")),
                  "no_indexed_documents");
    yyjson_doc_free(json);
    free(raw);
    refs_node(store, "demo", "Document", "demo.legacy", "docs/legacy.md", "{}");
    raw = cbm_document_refs_json(store, "demo", NULL, 0, 10);
    ASSERT_NOT_NULL(raw);
    json = yyjson_read(raw, strlen(raw), 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(json), "reason")),
                  "reindex_required");
    yyjson_doc_free(json);
    free(raw);
    refs_node(store, "demo", "Document", "demo.limited", "docs/limited.md",
              "{\"document_links\":{\"status\":\"limited\",\"reason\":\"read_failed\","
              "\"index_version\":1}}");
    raw = cbm_document_refs_json(store, "demo", NULL, 0, 10);
    ASSERT_NOT_NULL(raw);
    json = yyjson_read(raw, strlen(raw), 0);
    yyjson_val *root = yyjson_doc_get_root(json);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "status")), "limited");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "documents_unknown")), 1);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "documents_limited")), 1);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "total")), 0);
    yyjson_doc_free(json);
    free(raw);
    ASSERT_NULL(cbm_document_refs_json(store, "demo", NULL, 1, 10));
    cbm_store_close(store);
    PASS();
}

TEST(document_refs_limit_cap) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    int64_t doc = refs_node(store, "demo", "Document", "demo.doc", "docs/a.md", refs_ok);
    cbm_node_t targets[105] = {0};
    for (int i = 0; i < 105; i++) {
        char qn[64];
        snprintf(qn, sizeof(qn), "demo.function%03d", i);
        targets[i].id = refs_node(store, "demo", "Function", qn, "src/core.c", "{}");
        refs_edge(store, "demo", doc, targets[i].id, 105 - i, "document_links");
    }
    char *raw = cbm_document_refs_json(store, "demo", targets, 105, 1000);
    ASSERT_NOT_NULL(raw);
    yyjson_doc *json = yyjson_read(raw, strlen(raw), 0);
    yyjson_val *root = yyjson_doc_get_root(json);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "total")), 105);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "returned")), 100);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(root, "truncated")));
    yyjson_val *first = yyjson_arr_get(yyjson_obj_get(root, "references"), 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(first, "target"), "qualified_name")),
                  "demo.function104");
    yyjson_doc_free(json);
    free(raw);
    cbm_store_close(store);
    PASS();
}

SUITE(document_refs) {
    RUN_TEST(document_refs_order_evidence_and_dedup);
    RUN_TEST(document_refs_project_and_producer_isolation);
    RUN_TEST(document_refs_analysis_is_not_negative_proof);
    RUN_TEST(document_refs_limit_cap);
}
