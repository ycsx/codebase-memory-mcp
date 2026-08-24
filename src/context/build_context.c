#include "context/build_context.h"

#include "foundation/compat.h"
#include "foundation/constants.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson/yyjson.h>

enum {
    CONTEXT_DEFAULT_BUDGET = 4000,
    CONTEXT_MAX_BUDGET = 32000,
    CONTEXT_DEFAULT_LIMIT = 24,
    CONTEXT_NEIGHBOR_LIMIT = 8,
    CONTEXT_ESTIMATE_PER_NODE = 120,
};

enum {
    CONTEXT_REASON_EXACT_TARGET = 1U << 0,
    CONTEXT_REASON_TARGET_NAME = 1U << 1,
    CONTEXT_REASON_TARGET_FILE = 1U << 2,
    CONTEXT_REASON_TASK_TOKEN = 1U << 3,
    CONTEXT_REASON_INBOUND = 1U << 4,
    CONTEXT_REASON_OUTBOUND = 1U << 5,
    CONTEXT_REASON_DIFF_UNRESOLVED = 1U << 6,
    CONTEXT_REASON_RELATED_TEST = 1U << 7,
};

typedef struct {
    int score;
    int in_degree;
    int out_degree;
    unsigned reasons;
} context_rank_t;

static char *context_strdup(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t length = strlen(value);
    char *copy = malloc(length + 1U);
    if (copy) {
        memcpy(copy, value, length + 1U);
    }
    return copy;
}

static int clamp_budget(int budget) {
    if (budget <= 0) {
        return CONTEXT_DEFAULT_BUDGET;
    }
    if (budget > CONTEXT_MAX_BUDGET) {
        return CONTEXT_MAX_BUDGET;
    }
    return budget;
}

static bool valid_evidence_level(const char *level) {
    return !level || level[0] == '\0' || strcmp(level, "scout") == 0 ||
           strcmp(level, "analysis") == 0 || strcmp(level, "audit") == 0;
}

static bool is_test_node(const cbm_node_t *node) {
    return node && node->file_path && cbm_is_test_file_path(node->file_path);
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    for (const char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

static char *first_task_token(const char *task);

static const char *target_file_path(const char *target) {
    if (target && strncmp(target, "file:", 5) == 0) {
        return target + 5;
    }
    return target;
}

static context_rank_t rank_node(const cbm_node_t *node, const cbm_context_request_t *request,
                                int in_degree, int out_degree) {
    context_rank_t rank = {0};
    if (!node || !request) {
        return rank;
    }
    rank.in_degree = in_degree;
    rank.out_degree = out_degree;
    const char *target = request->target;
    const char *file_target = target_file_path(target);
    char *task_token = first_task_token(request->task);
    if (target && target[0] && node->qualified_name && strcmp(target, node->qualified_name) == 0) {
        rank.score += 10000;
        rank.reasons |= CONTEXT_REASON_EXACT_TARGET;
    }
    if (target && target[0] && node->name && strcmp(target, node->name) == 0) {
        rank.score += 5000;
        rank.reasons |= CONTEXT_REASON_TARGET_NAME;
    }
    if (file_target && file_target[0] && node->file_path &&
        strcmp(file_target, node->file_path) == 0) {
        rank.score += 4500;
        rank.reasons |= CONTEXT_REASON_TARGET_FILE;
    }
    if (task_token &&
        (contains_ci(node->qualified_name, task_token) || contains_ci(node->name, task_token) ||
         contains_ci(node->file_path, task_token))) {
        rank.score += 1000;
        rank.reasons |= CONTEXT_REASON_TASK_TOKEN;
    }
    free(task_token);

    /* Degree is a deterministic proxy for reachability and impact. Keep the
     * contribution bounded so an exact target always outranks a hotspot. */
    if (rank.in_degree > 0) {
        rank.score += rank.in_degree < 100 ? rank.in_degree * 3 : 300;
        rank.reasons |= CONTEXT_REASON_INBOUND;
    }
    if (rank.out_degree > 0) {
        rank.score += rank.out_degree < 100 ? rank.out_degree * 2 : 200;
        rank.reasons |= CONTEXT_REASON_OUTBOUND;
    }
    if (request->diff_ref && request->diff_ref[0]) {
        rank.reasons |= CONTEXT_REASON_DIFF_UNRESOLVED;
    }
    return rank;
}

static bool node_seen(const cbm_node_t *nodes, int count, int64_t id) {
    for (int i = 0; i < count; i++) {
        if (nodes[i].id == id) {
            return true;
        }
    }
    return false;
}

static void add_nullable_string(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                const char *value) {
    if (value) {
        yyjson_mut_obj_add_strcpy(doc, obj, key, value);
    } else {
        yyjson_mut_obj_add_null(doc, obj, key);
    }
}

static void add_node_json(yyjson_mut_doc *doc, yyjson_mut_val *obj, const cbm_node_t *node,
                          int in_degree, int out_degree) {
    yyjson_mut_obj_add_int(doc, obj, "id", node->id);
    add_nullable_string(doc, obj, "qualified_name", node->qualified_name);
    add_nullable_string(doc, obj, "label", node->label);
    add_nullable_string(doc, obj, "name", node->name);
    /* Keep the response aligned with the graph/search contracts so the UI can
     * resolve an evidence item back to its loaded GraphNode. */
    add_nullable_string(doc, obj, "file_path", node->file_path);
    yyjson_mut_obj_add_int(doc, obj, "start_line", node->start_line);
    yyjson_mut_obj_add_int(doc, obj, "end_line", node->end_line);
    yyjson_mut_obj_add_int(doc, obj, "in_degree", in_degree);
    yyjson_mut_obj_add_int(doc, obj, "out_degree", out_degree);
}

static void add_rank_json(yyjson_mut_doc *doc, yyjson_mut_val *obj, const context_rank_t *rank,
                          int rank_number) {
    yyjson_mut_obj_add_int(doc, obj, "rank", rank_number);
    yyjson_mut_obj_add_int(doc, obj, "score", rank->score);
    yyjson_mut_val *reasons = yyjson_mut_arr(doc);
    if (rank->reasons & CONTEXT_REASON_EXACT_TARGET) {
        yyjson_mut_arr_add_str(doc, reasons, "exact_target");
    }
    if (rank->reasons & CONTEXT_REASON_TARGET_NAME) {
        yyjson_mut_arr_add_str(doc, reasons, "target_name_match");
    }
    if (rank->reasons & CONTEXT_REASON_TARGET_FILE) {
        yyjson_mut_arr_add_str(doc, reasons, "target_file_match");
    }
    if (rank->reasons & CONTEXT_REASON_TASK_TOKEN) {
        yyjson_mut_arr_add_str(doc, reasons, "task_token_match");
    }
    if (rank->reasons & CONTEXT_REASON_INBOUND) {
        yyjson_mut_arr_add_str(doc, reasons, "inbound_degree");
    }
    if (rank->reasons & CONTEXT_REASON_OUTBOUND) {
        yyjson_mut_arr_add_str(doc, reasons, "outbound_degree");
    }
    if (rank->reasons & CONTEXT_REASON_DIFF_UNRESOLVED) {
        yyjson_mut_arr_add_str(doc, reasons, "diff_ref_unresolved");
    }
    if (rank->reasons & CONTEXT_REASON_RELATED_TEST) {
        yyjson_mut_arr_add_str(doc, reasons, "related_test");
    }
    if (rank->reasons == 0U) {
        yyjson_mut_arr_add_str(doc, reasons, "graph_fallback");
    }
    yyjson_mut_obj_add_val(doc, obj, "reasons", reasons);
}

static int compare_rank(const cbm_node_t *left_node, const context_rank_t *left,
                        const cbm_node_t *right_node, const context_rank_t *right) {
    if (left->score != right->score) {
        return left->score > right->score ? -1 : 1;
    }
    const char *left_qn = left_node->qualified_name ? left_node->qualified_name : "";
    const char *right_qn = right_node->qualified_name ? right_node->qualified_name : "";
    int qn_cmp = strcmp(left_qn, right_qn);
    if (qn_cmp != 0) {
        return qn_cmp;
    }
    const char *left_file = left_node->file_path ? left_node->file_path : "";
    const char *right_file = right_node->file_path ? right_node->file_path : "";
    int file_cmp = strcmp(left_file, right_file);
    if (file_cmp != 0) {
        return file_cmp;
    }
    if (left_node->id < right_node->id) {
        return -1;
    }
    if (left_node->id > right_node->id) {
        return 1;
    }
    return 0;
}

static void add_name_array(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, char **names,
                           int count) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_strcpy(doc, array, names[i]);
    }
    yyjson_mut_obj_add_val(doc, obj, key, array);
}

/* Emit one bounded evidence item. Keeping neighbor lookup and ownership in one
 * place prevents candidates and related tests from drifting into different
 * response shapes. */
static void add_evidence_json(cbm_store_t *store, yyjson_mut_doc *doc, yyjson_mut_val *evidence,
                              const cbm_node_t *node, const context_rank_t *rank, int rank_number,
                              int neighbor_limit, const char *evidence_kind) {
    int in_degree = 0;
    int out_degree = 0;
    cbm_store_node_degree(store, node->id, &in_degree, &out_degree);
    /* The caller normally has already computed degree values for ranked
     * candidates. Related tests are ranked lazily below, so their degree is
     * filled by the caller through rank->in_degree/out_degree. */
    if (rank) {
        in_degree = rank->in_degree;
        out_degree = rank->out_degree;
    }
    char **callers = NULL;
    char **callees = NULL;
    int caller_count = 0;
    int callee_count = 0;
    (void)cbm_store_node_neighbor_names(store, node->id, neighbor_limit, &callers, &caller_count,
                                        &callees, &callee_count);

    yyjson_mut_val *item = yyjson_mut_obj(doc);
    add_node_json(doc, item, node, in_degree, out_degree);
    if (rank) {
        add_rank_json(doc, item, rank, rank_number);
    }
    add_name_array(doc, item, "callers", callers, caller_count);
    add_name_array(doc, item, "callees", callees, callee_count);
    yyjson_mut_obj_add_strcpy(doc, item, "evidence", evidence_kind);
    yyjson_mut_arr_add_val(evidence, item);
    for (int i = 0; i < caller_count; i++) {
        free(callers[i]);
    }
    for (int i = 0; i < callee_count; i++) {
        free(callees[i]);
    }
    free(callers);
    free(callees);
}

static char *first_task_token(const char *task) {
    if (!task) {
        return NULL;
    }
    while (*task && !isalnum((unsigned char)*task) && *task != '_') {
        task++;
    }
    const char *end = task;
    while (*end && (isalnum((unsigned char)*end) || *end == '_' || *end == '-' || *end == '.')) {
        end++;
    }
    if (end == task) {
        return NULL;
    }
    size_t length = (size_t)(end - task);
    char *token = malloc(length + 1U);
    if (token) {
        memcpy(token, task, length);
        token[length] = '\0';
    }
    return token;
}

static int resolve_candidates(cbm_store_t *store, const char *project, const char *target,
                              const char *task, cbm_node_t **out_nodes) {
    *out_nodes = NULL;
    if (!store || !project) {
        return 0;
    }
    if (target && target[0]) {
        cbm_node_t exact = {0};
        if (strchr(target, '.') || strchr(target, ':')) {
            if (cbm_store_find_node_by_qn(store, project, target, &exact) == CBM_STORE_OK) {
                *out_nodes = malloc(sizeof(*out_nodes[0]));
                if (*out_nodes) {
                    (*out_nodes)[0] = exact;
                    return 1;
                }
                cbm_node_free_fields(&exact);
            }
        }
        int count = 0;
        if (cbm_store_find_nodes_by_name(store, project, target, out_nodes, &count) ==
                CBM_STORE_OK &&
            count > 0) {
            return count;
        }
        /* File targets may be a basename such as README.md, so do not
         * require a path separator before trying the exact file lookup. */
        const char *file_target = strncmp(target, "file:", 5) == 0 ? target + 5 : target;
        if (cbm_store_find_nodes_by_file(store, project, file_target, out_nodes, &count) ==
                CBM_STORE_OK &&
            count > 0) {
            return count;
        }
    }

    char *token = first_task_token(task);
    if (!token) {
        return 0;
    }
    int count = 0;
    (void)cbm_store_find_nodes_by_name(store, project, token, out_nodes, &count);
    free(token);
    return count;
}

char *cbm_context_build_json(cbm_store_t *store, const cbm_context_request_t *request,
                             cbm_context_stats_t *stats) {
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (!request || !request->project || !request->task || !store) {
        return context_strdup("{\"error\":\"project, task, and an indexed store are required\"}");
    }
    if (request->token_budget <= 0) {
        return context_strdup("{\"error\":\"token_budget must be greater than zero\"}");
    }
    if (!valid_evidence_level(request->evidence_level)) {
        return context_strdup("{\"error\":\"evidence_level must be scout, analysis, or audit\"}");
    }

    int budget = clamp_budget(request->token_budget);
    int evidence_limit = budget / CONTEXT_ESTIMATE_PER_NODE;
    if (evidence_limit > CONTEXT_DEFAULT_LIMIT) {
        evidence_limit = CONTEXT_DEFAULT_LIMIT;
    }
    int neighbor_limit = CONTEXT_NEIGHBOR_LIMIT;
    if (request->evidence_level && strcmp(request->evidence_level, "scout") == 0) {
        if (evidence_limit > 4) {
            evidence_limit = 4;
        }
        neighbor_limit = 4;
    } else if (request->evidence_level && strcmp(request->evidence_level, "audit") == 0) {
        neighbor_limit = 16;
    }

    cbm_node_t *candidates = NULL;
    int candidate_count =
        resolve_candidates(store, request->project, request->target, request->task, &candidates);
    context_rank_t *ranks =
        candidate_count > 0 ? calloc((size_t)candidate_count, sizeof(*ranks)) : NULL;
    if (candidate_count > 0 && !ranks) {
        cbm_store_free_nodes(candidates, candidate_count);
        return context_strdup("{\"error\":\"unable to allocate context ranking\"}");
    }
    for (int i = 0; i < candidate_count; i++) {
        int in_degree = 0;
        int out_degree = 0;
        cbm_store_node_degree(store, candidates[i].id, &in_degree, &out_degree);
        ranks[i] = rank_node(&candidates[i], request, in_degree, out_degree);
    }
    /* Insertion sort keeps the original order for exact ties; the comparator's
     * qualified-name/file/id fallback makes that order deterministic even when
     * SQLite changes its row traversal plan. */
    for (int i = 1; i < candidate_count; i++) {
        cbm_node_t node_key = candidates[i];
        context_rank_t rank_key = ranks[i];
        int j = i;
        while (j > 0 && compare_rank(&node_key, &rank_key, &candidates[j - 1], &ranks[j - 1]) < 0) {
            candidates[j] = candidates[j - 1];
            ranks[j] = ranks[j - 1];
            j--;
        }
        candidates[j] = node_key;
        ranks[j] = rank_key;
    }
    int candidate_budget_limit = evidence_limit > 0 ? evidence_limit : 1;
    if (candidate_budget_limit > CONTEXT_DEFAULT_LIMIT) {
        candidate_budget_limit = CONTEXT_DEFAULT_LIMIT;
    }
    int candidate_limit =
        candidate_count < candidate_budget_limit ? candidate_count : candidate_budget_limit;
    /* Even with a tiny budget, preserve two ambiguous candidates so callers
     * can disambiguate instead of receiving an arbitrary single choice. */
    if (candidate_count > 1 && candidate_limit < 2) {
        candidate_limit = 2;
    }
    bool ambiguous = candidate_count > 1;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "project", request->project);
    yyjson_mut_obj_add_strcpy(doc, root, "task", request->task);
    add_nullable_string(doc, root, "target", request->target);
    add_nullable_string(doc, root, "diff_ref", request->diff_ref);
    add_nullable_string(doc, root, "evidence_level",
                        request->evidence_level ? request->evidence_level : "analysis");

    yyjson_mut_val *resolved = yyjson_mut_obj(doc);
    if (candidate_count == 1) {
        int in_degree = 0;
        int out_degree = 0;
        cbm_store_node_degree(store, candidates[0].id, &in_degree, &out_degree);
        add_node_json(doc, resolved, &candidates[0], in_degree, out_degree);
        add_rank_json(doc, resolved, &ranks[0], 1);
        yyjson_mut_obj_add_val(doc, root, "resolved_target", resolved);
    } else {
        yyjson_mut_obj_add_null(doc, root, "resolved_target");
    }

    yyjson_mut_val *candidate_json = yyjson_mut_arr(doc);
    for (int i = 0; i < candidate_limit; i++) {
        if (!request->include_tests && is_test_node(&candidates[i])) {
            continue;
        }
        int in_degree = 0;
        int out_degree = 0;
        cbm_store_node_degree(store, candidates[i].id, &in_degree, &out_degree);
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        add_node_json(doc, item, &candidates[i], in_degree, out_degree);
        add_rank_json(doc, item, &ranks[i], i + 1);
        yyjson_mut_arr_add_val(candidate_json, item);
    }
    yyjson_mut_obj_add_val(doc, root, "candidates", candidate_json);

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    cbm_node_t selected[CONTEXT_DEFAULT_LIMIT];
    int selected_count = 0;
    int evidence_count = 0;
    int estimated_tokens = 0;
    for (int i = 0; i < candidate_count && evidence_count < evidence_limit; i++) {
        cbm_node_t *node = &candidates[i];
        if (!request->include_tests && is_test_node(node)) {
            continue;
        }
        if (node_seen(selected, selected_count, node->id)) {
            continue;
        }
        selected[selected_count++] = *node;
        add_evidence_json(store, doc, evidence, node, &ranks[i], i + 1, neighbor_limit,
                          "graph_node_and_direct_neighbors");
        evidence_count++;
        estimated_tokens += CONTEXT_ESTIMATE_PER_NODE;
    }

    /* Test discovery is intentionally conservative and deterministic. It
     * supplements graph evidence with conventional test symbol names without
     * treating a naming match as proof of coverage. */
    if (request->include_tests && evidence_count < evidence_limit) {
        for (int i = 0; i < candidate_count && evidence_count < evidence_limit; i++) {
            const char *name = candidates[i].name;
            if (!name || !name[0]) {
                continue;
            }
            char test_names[3][CBM_SZ_256];
            snprintf(test_names[0], sizeof(test_names[0]), "test_%s", name);
            snprintf(test_names[1], sizeof(test_names[1]), "%s_test", name);
            snprintf(test_names[2], sizeof(test_names[2]), "Test%s", name);
            for (int pattern = 0; pattern < 3 && evidence_count < evidence_limit; pattern++) {
                cbm_node_t *test_nodes = NULL;
                int test_count = 0;
                if (cbm_store_find_nodes_by_name(store, request->project, test_names[pattern],
                                                 &test_nodes, &test_count) != CBM_STORE_OK) {
                    continue;
                }
                for (int j = 0; j < test_count && evidence_count < evidence_limit; j++) {
                    cbm_node_t *test = &test_nodes[j];
                    if (!is_test_node(test) || node_seen(selected, selected_count, test->id)) {
                        continue;
                    }
                    int in_degree = 0;
                    int out_degree = 0;
                    cbm_store_node_degree(store, test->id, &in_degree, &out_degree);
                    context_rank_t test_rank = {
                        .score = 0,
                        .in_degree = in_degree,
                        .out_degree = out_degree,
                        .reasons = CONTEXT_REASON_RELATED_TEST,
                    };
                    selected[selected_count++] = *test;
                    add_evidence_json(store, doc, evidence, test, &test_rank,
                                      candidate_count + evidence_count + 1, neighbor_limit,
                                      "related_test_and_direct_neighbors");
                    evidence_count++;
                    estimated_tokens += CONTEXT_ESTIMATE_PER_NODE;
                }
                cbm_store_free_nodes(test_nodes, test_count);
            }
        }
    }
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);

    yyjson_mut_val *documentation = yyjson_mut_arr(doc);
    bool documentation_truncated = false;
    if (request->include_docs) {
        char **doc_paths = NULL;
        int doc_count = 0;
        if (cbm_store_find_architecture_docs(store, request->project, &doc_paths, &doc_count) ==
            CBM_STORE_OK) {
            int doc_limit = evidence_limit;
            if (doc_count > doc_limit) {
                documentation_truncated = true;
            }
            for (int i = 0; i < doc_count && i < doc_limit; i++) {
                if (doc_paths[i]) {
                    yyjson_mut_arr_add_strcpy(doc, documentation, doc_paths[i]);
                    free(doc_paths[i]);
                }
            }
            for (int i = doc_limit; i < doc_count; i++) {
                free(doc_paths[i]);
            }
            free(doc_paths);
        }
    }
    yyjson_mut_obj_add_val(doc, root, "documentation", documentation);

    bool budget_truncated = candidate_count > candidate_limit || evidence_count < candidate_count ||
                            documentation_truncated;
    bool resolution_incomplete = candidate_count != 1;
    yyjson_mut_val *budget_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, budget_obj, "requested_tokens", budget);
    yyjson_mut_obj_add_int(doc, budget_obj, "estimated_tokens", estimated_tokens);
    yyjson_mut_obj_add_int(doc, budget_obj, "evidence_limit", evidence_limit);
    yyjson_mut_obj_add_int(doc, budget_obj, "neighbor_limit", neighbor_limit);
    yyjson_mut_obj_add_bool(doc, budget_obj, "truncated", budget_truncated);
    yyjson_mut_obj_add_val(doc, root, "budget", budget_obj);

    yyjson_mut_val *limitations = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(
        doc, limitations,
        "Graph evidence is deterministic but best-effort; dynamic calls and reflection may be "
        "absent.");
    if (candidate_count == 0) {
        yyjson_mut_arr_add_str(
            doc, limitations,
            "No unique target was resolved; use search_graph to find an exact qualified name.");
    } else if (ambiguous) {
        yyjson_mut_arr_add_str(
            doc, limitations,
            "The target is ambiguous; choose one candidate before relying on impact conclusions.");
    }
    if (!request->include_docs) {
        yyjson_mut_arr_add_str(doc, limitations, "Documentation evidence was not requested.");
    }
    if (request->diff_ref && request->diff_ref[0]) {
        yyjson_mut_arr_add_str(
            doc, limitations,
            "The diff reference is recorded for this context; use detect_changes for full Git "
            "diff evidence.");
    }
    if (budget_truncated) {
        yyjson_mut_arr_add_str(doc, limitations,
                               "Evidence was trimmed to the requested token budget.");
    }
    if (documentation_truncated) {
        yyjson_mut_arr_add_str(doc, limitations,
                               "Documentation evidence was trimmed to the requested token budget.");
    }
    yyjson_mut_obj_add_val(doc, root, "limitations", limitations);

    size_t length = 0;
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &length);
    yyjson_mut_doc_free(doc);
    cbm_store_free_nodes(candidates, candidate_count);
    free(ranks);
    if (stats) {
        stats->returned = evidence_count;
        stats->total = candidate_count;
        stats->estimated_tokens = estimated_tokens;
        stats->budget_truncated = budget_truncated;
        stats->resolution_incomplete = resolution_incomplete;
        stats->truncated = budget_truncated || resolution_incomplete;
    }
    return json;
}
