#include "test_framework.h"
#include "context/build_context.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

static cbm_store_t *context_document_fixture(int references) {
    cbm_store_t *store = cbm_store_open_memory();
    if (!store) {
        return NULL;
    }
    cbm_store_upsert_project(store, "demo", "/demo");
    cbm_node_t target = {.project = "demo",
                         .label = "Function",
                         .name = "compute",
                         .qualified_name = "demo.core.compute",
                         .file_path = "src/core.c",
                         .start_line = 2,
                         .end_line = 5,
                         .properties_json = "{}"};
    int64_t target_id = cbm_store_upsert_node(store, &target);
    for (int i = 0; i < references; i++) {
        char qn[128];
        char path[128];
        snprintf(qn, sizeof(qn), "demo.docs.guide%d", i);
        snprintf(path, sizeof(path), "docs/guide%d.md", i);
        cbm_node_t source = {.project = "demo",
                             .label = "Document",
                             .name = "Guide",
                             .qualified_name = qn,
                             .file_path = path,
                             .start_line = 1,
                             .end_line = 20,
                             .properties_json =
                                 "{\"document_links\":{\"status\":\"ok\",\"index_version\":1}}"};
        int64_t source_id = cbm_store_upsert_node(store, &source);
        cbm_edge_t edge = {
            .project = "demo",
            .source_id = source_id,
            .target_id = target_id,
            .type = "REFERENCES",
            .properties_json =
                "{\"producer\":\"document_links\",\"source\":\"symbol_match\","
                "\"confidence\":1.0,\"document_span\":{\"start_line\":7,\"end_line\":7},"
                "\"matched_text\":\"demo.core.compute\","
                "\"target_qualified_name\":\"demo.core.compute\",\"index_version\":1}"};
        cbm_store_insert_edge(store, &edge);
    }
    return store;
}

static yyjson_doc *context_document_request(cbm_store_t *store, bool include_docs, int budget,
                                            cbm_context_stats_t *stats) {
    cbm_context_request_t request = {.project = "demo",
                                     .task = "inspect compute",
                                     .target = "demo.core.compute",
                                     .token_budget = budget,
                                     .include_docs = include_docs};
    char *json = cbm_context_build_json(store, &request, stats);
    if (!json) {
        return NULL;
    }
    yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
    free(json);
    return parsed;
}

TEST(context_documents_include_evidence_not_callers) {
    cbm_store_t *store = context_document_fixture(1);
    ASSERT_NOT_NULL(store);
    cbm_context_stats_t stats;
    yyjson_doc *json = context_document_request(store, true, 4000, &stats);
    ASSERT_NOT_NULL(json);
    yyjson_val *root = yyjson_doc_get_root(json);
    yyjson_val *docs = yyjson_obj_get(root, "documentation_references");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(docs, "returned")), 1);
    yyjson_val *reference = yyjson_arr_get_first(yyjson_obj_get(docs, "references"));
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(reference, "source"), "file_path")),
                  "docs/guide0.md");
    yyjson_val *properties = yyjson_obj_get(reference, "properties");
    ASSERT_EQ(
        yyjson_get_int(yyjson_obj_get(yyjson_obj_get(properties, "document_span"), "start_line")),
        7);
    yyjson_val *evidence = yyjson_arr_get_first(yyjson_obj_get(root, "evidence"));
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(evidence, "callers")), 0);
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(evidence, "callees")), 0);
    ASSERT(stats.estimated_tokens > 120);
    ASSERT(stats.estimated_tokens <= 4000);
    yyjson_doc_free(json);
    cbm_store_close(store);
    PASS();
}

TEST(context_documents_opt_out) {
    cbm_store_t *store = context_document_fixture(1);
    ASSERT_NOT_NULL(store);
    cbm_context_stats_t stats;
    yyjson_doc *json = context_document_request(store, false, 4000, &stats);
    ASSERT_NOT_NULL(json);
    yyjson_val *root = yyjson_doc_get_root(json);
    yyjson_val *docs = yyjson_obj_get(root, "documentation_references");
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(docs, "references")), 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(docs, "status")), "not_requested");
    ASSERT_EQ(stats.estimated_tokens, 120);
    yyjson_doc_free(json);
    cbm_store_close(store);
    PASS();
}

TEST(context_documents_share_budget_and_report_truncation) {
    cbm_store_t *store = context_document_fixture(1);
    ASSERT_NOT_NULL(store);
    cbm_context_stats_t stats;
    yyjson_doc *json = context_document_request(store, true, 120, &stats);
    ASSERT_NOT_NULL(json);
    yyjson_val *docs = yyjson_obj_get(yyjson_doc_get_root(json), "documentation_references");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(docs, "returned")), 0);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(docs, "total")), 1);
    ASSERT(yyjson_get_bool(yyjson_obj_get(docs, "truncated")));
    ASSERT(yyjson_get_bool(yyjson_obj_get(docs, "budget_truncated")));
    ASSERT_EQ(stats.estimated_tokens, 120);
    ASSERT(stats.budget_truncated);
    yyjson_doc_free(json);
    cbm_store_close(store);
    PASS();
}

TEST(context_documents_count_limit_and_determinism) {
    cbm_store_t *store = context_document_fixture(12);
    ASSERT_NOT_NULL(store);
    cbm_context_stats_t stats;
    yyjson_doc *json = context_document_request(store, true, 32000, &stats);
    yyjson_doc *repeat = context_document_request(store, true, 32000, NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(repeat);
    yyjson_val *docs = yyjson_obj_get(yyjson_doc_get_root(json), "documentation_references");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(docs, "returned")), 8);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(docs, "total")), 12);
    ASSERT(yyjson_get_bool(yyjson_obj_get(docs, "truncated")));
    ASSERT(!yyjson_get_bool(yyjson_obj_get(docs, "budget_truncated")));
    char *first = yyjson_write(json, 0, NULL);
    char *second = yyjson_write(repeat, 0, NULL);
    ASSERT_STR_EQ(first, second);
    free(first);
    free(second);
    yyjson_doc_free(json);
    yyjson_doc_free(repeat);
    cbm_store_close(store);
    PASS();
}

SUITE(context_documents) {
    RUN_TEST(context_documents_include_evidence_not_callers);
    RUN_TEST(context_documents_opt_out);
    RUN_TEST(context_documents_share_budget_and_report_truncation);
    RUN_TEST(context_documents_count_limit_and_determinism);
}
