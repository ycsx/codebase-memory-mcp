#include "test_framework.h"
#include "test_helpers.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

#include <stdatomic.h>

static char document_root[512];

static cbm_gbuf_t *document_fixture(const char *text) {
    char *root = th_mktempdir("cbm_doclinks");
    if (!root) {
        return NULL;
    }
    snprintf(document_root, sizeof(document_root), "%s", root);
    if (th_write_file(TH_PATH(document_root, "docs/guide.md"), text) != 0) {
        return NULL;
    }
    cbm_gbuf_t *gb = cbm_gbuf_new("demo", document_root);
    if (!gb) {
        return NULL;
    }
    cbm_gbuf_upsert_node(gb, "Document", "guide.md", "demo.docs.guide.__document__",
                         "docs/guide.md", 1, 0, "{\"format\":\"markdown\"}");
    int64_t section = cbm_gbuf_upsert_node(gb, "Section", "Usage", "demo.docs.guide.Usage",
                                           "docs/guide.md", 3, 100, "{}");
    const cbm_gbuf_node_t *doc = cbm_gbuf_find_by_qn(gb, "demo.docs.guide.__document__");
    cbm_gbuf_insert_edge(gb, doc->id, section, "CONTAINS_SECTION", "{}");
    cbm_gbuf_upsert_node(gb, "File", "core.c", "demo.src.core.__file__", "src/core.c", 0, 0, "{}");
    cbm_gbuf_upsert_node(gb, "Function", "compute", "demo.src.core.compute", "src/core.c", 2, 5,
                         "{}");
    return gb;
}

static int document_pass(cbm_gbuf_t *gb) {
    atomic_int cancelled = 0;
    cbm_pipeline_ctx_t ctx = {.project_name = "demo",
                              .repo_path = document_root,
                              .gbuf = gb,
                              .cancelled = &cancelled,
                              .mode = CBM_MODE_FULL};
    return cbm_pipeline_pass_document_links(&ctx);
}

static void document_fixture_free(cbm_gbuf_t *gb) {
    cbm_gbuf_free(gb);
    th_rmtree(document_root);
}

static const cbm_gbuf_edge_t *document_edge(cbm_gbuf_t *gb, const char *source,
                                            const char *target) {
    const cbm_gbuf_node_t *src = cbm_gbuf_find_by_qn(gb, source);
    const cbm_gbuf_node_t *dst = cbm_gbuf_find_by_qn(gb, target);
    if (!src || !dst) {
        return NULL;
    }
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    cbm_gbuf_find_edges_by_source_type(gb, src->id, "REFERENCES", &edges, &count);
    for (int i = 0; i < count; i++) {
        if (edges[i]->target_id == dst->id) {
            return edges[i];
        }
    }
    return NULL;
}

TEST(document_links_explicit_path_and_symbol_evidence) {
    cbm_gbuf_t *gb = document_fixture("[Core](../src/core.c)\n\n# Usage\n"
                                      "`demo.src.core.compute`\n");
    ASSERT_NOT_NULL(gb);
    ASSERT_EQ(document_pass(gb), 2);
    const cbm_gbuf_edge_t *edge =
        document_edge(gb, "demo.docs.guide.__document__", "demo.src.core.__file__");
    ASSERT_NOT_NULL(edge);
    yyjson_doc *json = yyjson_read(edge->properties_json, strlen(edge->properties_json), 0);
    ASSERT_NOT_NULL(json);
    yyjson_val *props = yyjson_doc_get_root(json);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(props, "producer")), "document_links");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(props, "source")), "explicit_link");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(props, "matched_text")), "../src/core.c");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(yyjson_obj_get(props, "document_span"), "start_line")),
              1);
    yyjson_doc_free(json);
    edge = document_edge(gb, "demo.docs.guide.Usage", "demo.src.core.compute");
    ASSERT_NOT_NULL(edge);
    json = yyjson_read(edge->properties_json, strlen(edge->properties_json), 0);
    ASSERT_NOT_NULL(json);
    props = yyjson_doc_get_root(json);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(props, "source")), "symbol_match");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(yyjson_obj_get(props, "document_span"), "start_line")),
              4);
    yyjson_doc_free(json);
    document_fixture_free(gb);
    PASS();
}

TEST(document_links_path_match_and_first_evidence) {
    cbm_gbuf_t *gb = document_fixture("`src/core.c`\n`src/core.c`\n");
    ASSERT_NOT_NULL(gb);
    ASSERT_EQ(document_pass(gb), 1);
    const cbm_gbuf_edge_t *edge =
        document_edge(gb, "demo.docs.guide.__document__", "demo.src.core.__file__");
    ASSERT_NOT_NULL(edge);
    yyjson_doc *json = yyjson_read(edge->properties_json, strlen(edge->properties_json), 0);
    ASSERT_NOT_NULL(json);
    yyjson_val *props = yyjson_doc_get_root(json);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(props, "source")), "path_match");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(yyjson_obj_get(props, "document_span"), "start_line")),
              1);
    yyjson_doc_free(json);
    ASSERT_EQ(document_pass(gb), 1);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 1);
    document_fixture_free(gb);
    PASS();
}

TEST(document_links_do_not_guess_short_or_ambiguous_names) {
    cbm_gbuf_t *gb = document_fixture("`compute`\n`src/core.c`\n"
                                      "[Explicit](../src/core.c)\n");
    ASSERT_NOT_NULL(gb);
    cbm_gbuf_upsert_node(gb, "File", "core.c", "demo.docs.src.core.__file__", "docs/src/core.c", 0,
                         0, "{}");
    cbm_gbuf_upsert_node(gb, "Function", "compute", "demo.other.compute", "other.c", 1, 3, "{}");
    ASSERT_EQ(document_pass(gb), 1);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 1);
    ASSERT_NOT_NULL(document_edge(gb, "demo.docs.guide.Usage", "demo.src.core.__file__"));
    document_fixture_free(gb);
    PASS();
}

TEST(document_links_ignore_nonreferences) {
    cbm_gbuf_t *gb =
        document_fixture("[External](https://example.com/src/core.c)\n"
                         "[Escape](../../src/core.c)\n"
                         "![Image](../src/core.c)\n"
                         "```c\n`demo.src.core.compute`\n[Hidden](../src/core.c)\n```\n"
                         "~~~\n`src/core.c`\n~~~\n"
                         "[Unknown](../missing.c)\n"
                         "`unknown.qualified.symbol`\n");
    ASSERT_NOT_NULL(gb);
    ASSERT_EQ(document_pass(gb), 0);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 0);
    document_fixture_free(gb);
    PASS();
}

TEST(document_links_ignore_comments_and_multiline_code) {
    cbm_gbuf_t *gb =
        document_fixture("<!-- [Core](../src/core.c) -->\n"
                         "<!--\n`demo.src.core.compute`\n-->\n"
                         "`` example\n[Core](../src/core.c)\n`demo.src.core.compute`\n``\n");
    ASSERT_NOT_NULL(gb);
    ASSERT_EQ(document_pass(gb), 0);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 0);
    document_fixture_free(gb);
    PASS();
}

TEST(document_links_rebuild_preserves_unrelated_edges) {
    cbm_gbuf_t *gb = document_fixture("[Core](../src/core.c)\n");
    ASSERT_NOT_NULL(gb);
    ASSERT_EQ(document_pass(gb), 1);
    const cbm_gbuf_node_t *doc = cbm_gbuf_find_by_qn(gb, "demo.docs.guide.__document__");
    const cbm_gbuf_node_t *symbol = cbm_gbuf_find_by_qn(gb, "demo.src.core.compute");
    ASSERT_GT(
        cbm_gbuf_insert_edge(gb, doc->id, symbol->id, "REFERENCES", "{\"producer\":\"manual\"}"),
        0);
    ASSERT_EQ(th_write_file(TH_PATH(document_root, "docs/guide.md"), "No references.\n"), 0);
    ASSERT_EQ(document_pass(gb), 0);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 1);
    ASSERT_NOT_NULL(document_edge(gb, "demo.docs.guide.__document__", "demo.src.core.compute"));
    const cbm_gbuf_edge_t **incoming = NULL;
    int count = 0;
    const cbm_gbuf_node_t *file = cbm_gbuf_find_by_qn(gb, "demo.src.core.__file__");
    cbm_gbuf_find_edges_by_target_type(gb, file->id, "REFERENCES", &incoming, &count);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(th_write_file(TH_PATH(document_root, "docs/guide.md"), "`src/core.c`\n"), 0);
    ASSERT_EQ(document_pass(gb), 1);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 2);
    document_fixture_free(gb);
    PASS();
}

TEST(document_links_deleted_target_disappears) {
    cbm_gbuf_t *gb = document_fixture("`demo.src.core.compute`\n");
    ASSERT_NOT_NULL(gb);
    ASSERT_EQ(document_pass(gb), 1);
    cbm_gbuf_delete_by_file(gb, "src/core.c");
    ASSERT_EQ(document_pass(gb), 0);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 0);
    document_fixture_free(gb);
    PASS();
}

static bool document_test_delete_predicate(const cbm_gbuf_edge_t *edge, void *userdata) {
    return edge->target_id == *(int64_t *)userdata && strcmp(edge->type, "REFERENCES") == 0;
}

TEST(document_links_selective_delete_indexes) {
    cbm_gbuf_t *gb = document_fixture("");
    ASSERT_NOT_NULL(gb);
    int64_t doc = cbm_gbuf_find_by_qn(gb, "demo.docs.guide.__document__")->id;
    int64_t file = cbm_gbuf_find_by_qn(gb, "demo.src.core.__file__")->id;
    int64_t symbol = cbm_gbuf_find_by_qn(gb, "demo.src.core.compute")->id;
    ASSERT_GT(cbm_gbuf_insert_edge(gb, doc, file, "REFERENCES", "{}"), 0);
    ASSERT_GT(cbm_gbuf_insert_edge(gb, doc, symbol, "REFERENCES", "{}"), 0);
    ASSERT_GT(cbm_gbuf_insert_edge(gb, doc, file, "OTHER", "{}"), 0);
    ASSERT_EQ(cbm_gbuf_delete_edges_if(gb, document_test_delete_predicate, &file), 1);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 1);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "OTHER"), 1);
    ASSERT_GT(cbm_gbuf_insert_edge(gb, doc, file, "REFERENCES", "{}"), 0);
    ASSERT_EQ(cbm_gbuf_edge_count_by_type(gb, "REFERENCES"), 2);
    ASSERT_EQ(cbm_gbuf_delete_edges_if(gb, document_test_delete_predicate, &file), 1);
    document_fixture_free(gb);
    PASS();
}

static int document_run_index(const char *repo, const char *db) {
    cbm_pipeline_t *pipeline = cbm_pipeline_new(repo, db, CBM_MODE_FULL);
    if (!pipeline) {
        return -1;
    }
    cbm_pipeline_set_project_name(pipeline, "doc-incr");
    int rc = cbm_pipeline_run(pipeline);
    cbm_pipeline_free(pipeline);
    return rc;
}

static int document_stored_references(const char *db) {
    cbm_store_t *store = cbm_store_open_path(db);
    if (!store) {
        return -1;
    }
    cbm_node_t *docs = NULL;
    int count = 0;
    int total = 0;
    if (cbm_store_find_nodes_by_label(store, "doc-incr", "Document", &docs, &count) !=
        CBM_STORE_OK) {
        cbm_store_close(store);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        cbm_edge_t *edges = NULL;
        int n = 0;
        cbm_store_find_edges_by_source_type(store, docs[i].id, "REFERENCES", &edges, &n);
        total += n;
        cbm_store_free_edges(edges, n);
    }
    cbm_store_free_nodes(docs, count);
    cbm_store_close(store);
    return total;
}

TEST(document_links_full_and_incremental_lifecycle) {
    char *tmp = th_mktempdir("cbm_doclink_index");
    ASSERT_NOT_NULL(tmp);
    char root[512];
    snprintf(root, sizeof(root), "%s", tmp);
    char repo[640];
    char db[640];
    snprintf(repo, sizeof(repo), "%s/repo", root);
    snprintf(db, sizeof(db), "%s/graph.db", root);
    ASSERT_EQ(th_write_file(TH_PATH(repo, "README.md"),
                            "[Core](src/core.py)\n`doc-incr.src.core.compute`\n"),
              0);
    ASSERT_EQ(th_write_file(TH_PATH(repo, "src/core.py"), "def compute():\n    return 1\n"), 0);
    ASSERT_EQ(document_run_index(repo, db), 0);
    ASSERT_EQ(document_stored_references(db), 2);
    /* Older indexes have file hashes but no document-reference pass version. */
    cbm_store_t *legacy = cbm_store_open_path(db);
    ASSERT_NOT_NULL(legacy);
    ASSERT_EQ(cbm_store_exec(legacy, "UPDATE nodes SET properties='{}' WHERE label='Document';"),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_store_exec(legacy, "DELETE FROM edges WHERE type='REFERENCES';"), CBM_STORE_OK);
    cbm_store_close(legacy);
    ASSERT_EQ(document_run_index(repo, db), 0);
    ASSERT_EQ(document_stored_references(db), 2);
    legacy = cbm_store_open_path(db);
    ASSERT_NOT_NULL(legacy);
    ASSERT_EQ(
        cbm_store_exec(legacy,
                       "UPDATE nodes SET properties='{\"document_links\":{\"status\":\"limited\","
                       "\"reason\":\"read_failed\",\"index_version\":1}}' WHERE label='Document';"),
        CBM_STORE_OK);
    ASSERT_EQ(cbm_store_exec(legacy, "DELETE FROM edges WHERE type='REFERENCES';"), CBM_STORE_OK);
    cbm_store_close(legacy);
    ASSERT_EQ(document_run_index(repo, db), 0);
    ASSERT_EQ(document_stored_references(db), 2);
    ASSERT_EQ(document_run_index(repo, db), 0);
    ASSERT_EQ(document_stored_references(db), 2);
    ASSERT_EQ(
        th_write_file(TH_PATH(repo, "src/core.py"), "def renamed_compute():\n    return 42\n"), 0);
    ASSERT_EQ(document_run_index(repo, db), 0);
    ASSERT_EQ(document_stored_references(db), 1);
    ASSERT_EQ(th_write_file(TH_PATH(repo, "README.md"), "References removed.\n"), 0);
    ASSERT_EQ(document_run_index(repo, db), 0);
    ASSERT_EQ(document_stored_references(db), 0);
    th_rmtree(root);
    PASS();
}

SUITE(document_links) {
    RUN_TEST(document_links_explicit_path_and_symbol_evidence);
    RUN_TEST(document_links_path_match_and_first_evidence);
    RUN_TEST(document_links_do_not_guess_short_or_ambiguous_names);
    RUN_TEST(document_links_ignore_nonreferences);
    RUN_TEST(document_links_ignore_comments_and_multiline_code);
    RUN_TEST(document_links_rebuild_preserves_unrelated_edges);
    RUN_TEST(document_links_deleted_target_disappears);
    RUN_TEST(document_links_selective_delete_indexes);
    RUN_TEST(document_links_full_and_incremental_lifecycle);
}
