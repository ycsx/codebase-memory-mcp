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

typedef struct {
    cbm_node_t node;
    context_rank_t rank;
} context_ranked_node_t;

typedef struct {
    cbm_node_t *candidates;
    context_rank_t *ranks;
    int candidate_count;
    int evidence_limit;
    int neighbor_limit;
    cbm_node_t selected[CONTEXT_DEFAULT_LIMIT * 2];
    int selected_count;
    int evidence_count;
    int estimated_tokens;
} context_emit_state_t;

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

static bool add_nullable_string(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                const char *value) {
    if (value) {
        return yyjson_mut_obj_add_strcpy(doc, obj, key, value);
    }
    return yyjson_mut_obj_add_null(doc, obj, key);
}

static bool add_node_json(yyjson_mut_doc *doc, yyjson_mut_val *obj, const cbm_node_t *node,
                          int in_degree, int out_degree) {
    bool ok = yyjson_mut_obj_add_int(doc, obj, "id", node->id);
    ok = ok && add_nullable_string(doc, obj, "qualified_name", node->qualified_name);
    ok = ok && add_nullable_string(doc, obj, "label", node->label);
    ok = ok && add_nullable_string(doc, obj, "name", node->name);
    /* Keep the response aligned with the graph/search contracts so the UI can
     * resolve an evidence item back to its loaded GraphNode. */
    ok = ok && add_nullable_string(doc, obj, "file_path", node->file_path);
    ok = ok && yyjson_mut_obj_add_int(doc, obj, "start_line", node->start_line);
    ok = ok && yyjson_mut_obj_add_int(doc, obj, "end_line", node->end_line);
    ok = ok && yyjson_mut_obj_add_int(doc, obj, "in_degree", in_degree);
    ok = ok && yyjson_mut_obj_add_int(doc, obj, "out_degree", out_degree);
    return ok;
}

static bool add_rank_json(yyjson_mut_doc *doc, yyjson_mut_val *obj, const context_rank_t *rank,
                          int rank_number) {
    bool ok = yyjson_mut_obj_add_int(doc, obj, "rank", rank_number);
    ok = ok && yyjson_mut_obj_add_int(doc, obj, "score", rank->score);
    yyjson_mut_val *reasons = yyjson_mut_arr(doc);
    if (!ok || !reasons) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_EXACT_TARGET) &&
        !yyjson_mut_arr_add_str(doc, reasons, "exact_target")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_TARGET_NAME) &&
        !yyjson_mut_arr_add_str(doc, reasons, "target_name_match")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_TARGET_FILE) &&
        !yyjson_mut_arr_add_str(doc, reasons, "target_file_match")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_TASK_TOKEN) &&
        !yyjson_mut_arr_add_str(doc, reasons, "task_token_match")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_INBOUND) &&
        !yyjson_mut_arr_add_str(doc, reasons, "inbound_degree")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_OUTBOUND) &&
        !yyjson_mut_arr_add_str(doc, reasons, "outbound_degree")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_DIFF_UNRESOLVED) &&
        !yyjson_mut_arr_add_str(doc, reasons, "diff_ref_unresolved")) {
        return false;
    }
    if ((rank->reasons & CONTEXT_REASON_RELATED_TEST) &&
        !yyjson_mut_arr_add_str(doc, reasons, "related_test")) {
        return false;
    }
    if (rank->reasons == 0U && !yyjson_mut_arr_add_str(doc, reasons, "graph_fallback")) {
        return false;
    }
    return yyjson_mut_obj_add_val(doc, obj, "reasons", reasons);
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

static int compare_ranked_nodes(const void *left, const void *right) {
    const context_ranked_node_t *a = left;
    const context_ranked_node_t *b = right;
    return compare_rank(&a->node, &a->rank, &b->node, &b->rank);
}

static bool add_name_array(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, char **names,
                           int count) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    if (!array) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < count; i++) {
        ok = ok && yyjson_mut_arr_add_strcpy(doc, array, names[i]);
    }
    return ok && yyjson_mut_obj_add_val(doc, obj, key, array);
}

/* Emit one bounded evidence item. Keeping neighbor lookup and ownership in one
 * place prevents candidates and related tests from drifting into different
 * response shapes. */
static bool add_evidence_json(cbm_store_t *store, yyjson_mut_doc *doc, yyjson_mut_val *evidence,
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
    int neighbor_rc = cbm_store_node_neighbor_names(store, node->id, neighbor_limit, &callers,
                                                    &caller_count, &callees, &callee_count);

    yyjson_mut_val *item = yyjson_mut_obj(doc);
    bool ok = item && add_node_json(doc, item, node, in_degree, out_degree);
    if (rank) {
        ok = ok && add_rank_json(doc, item, rank, rank_number);
    }
    ok = ok && add_name_array(doc, item, "callers", callers, caller_count);
    ok = ok && add_name_array(doc, item, "callees", callees, callee_count);
    ok =
        ok && yyjson_mut_obj_add_bool(doc, item, "neighbors_complete", neighbor_rc == CBM_STORE_OK);
    ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "evidence", evidence_kind);
    ok = ok && yyjson_mut_arr_add_val(evidence, item);
    for (int i = 0; i < caller_count; i++) {
        free(callers[i]);
    }
    for (int i = 0; i < callee_count; i++) {
        free(callees[i]);
    }
    free(callers);
    free(callees);
    return ok;
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

static void free_emit_state(context_emit_state_t *state) {
    if (!state) {
        return;
    }
    cbm_store_free_nodes(state->candidates, state->candidate_count);
    free(state->ranks);
    state->candidates = NULL;
    state->ranks = NULL;
}

static bool prepare_candidates(cbm_store_t *store, const cbm_context_request_t *request,
                               context_emit_state_t *state) {
    state->candidate_count = resolve_candidates(store, request->project, request->target,
                                                request->task, &state->candidates);
    if (state->candidate_count == 0) {
        return true;
    }
    state->ranks = calloc((size_t)state->candidate_count, sizeof(*state->ranks));
    if (!state->ranks) {
        return false;
    }
    for (int i = 0; i < state->candidate_count; i++) {
        int in_degree = 0;
        int out_degree = 0;
        cbm_store_node_degree(store, state->candidates[i].id, &in_degree, &out_degree);
        state->ranks[i] = rank_node(&state->candidates[i], request, in_degree, out_degree);
    }
    if (state->candidate_count < 2) {
        return true;
    }

    context_ranked_node_t *ranked = malloc((size_t)state->candidate_count * sizeof(*ranked));
    if (!ranked) {
        return false;
    }
    for (int i = 0; i < state->candidate_count; i++) {
        ranked[i].node = state->candidates[i];
        ranked[i].rank = state->ranks[i];
    }
    qsort(ranked, (size_t)state->candidate_count, sizeof(*ranked), compare_ranked_nodes);
    for (int i = 0; i < state->candidate_count; i++) {
        state->candidates[i] = ranked[i].node;
        state->ranks[i] = ranked[i].rank;
    }
    free(ranked);
    return true;
}

static bool add_resolved_target_json(cbm_store_t *store, yyjson_mut_doc *doc, yyjson_mut_val *root,
                                     const context_emit_state_t *state) {
    if (state->candidate_count != 1) {
        return yyjson_mut_obj_add_null(doc, root, "resolved_target");
    }
    int in_degree = 0;
    int out_degree = 0;
    cbm_store_node_degree(store, state->candidates[0].id, &in_degree, &out_degree);
    yyjson_mut_val *resolved = yyjson_mut_obj(doc);
    return resolved && add_node_json(doc, resolved, &state->candidates[0], in_degree, out_degree) &&
           add_rank_json(doc, resolved, &state->ranks[0], 1) &&
           yyjson_mut_obj_add_val(doc, root, "resolved_target", resolved);
}

static bool add_candidates_json(cbm_store_t *store, yyjson_mut_doc *doc, yyjson_mut_val *root,
                                const cbm_context_request_t *request,
                                const context_emit_state_t *state, int candidate_limit) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    if (!array) {
        return false;
    }
    for (int i = 0; i < candidate_limit; i++) {
        if (!request->include_tests && is_test_node(&state->candidates[i])) {
            continue;
        }
        int in_degree = 0;
        int out_degree = 0;
        cbm_store_node_degree(store, state->candidates[i].id, &in_degree, &out_degree);
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        if (!item || !add_node_json(doc, item, &state->candidates[i], in_degree, out_degree) ||
            !add_rank_json(doc, item, &state->ranks[i], i + 1) ||
            !yyjson_mut_arr_add_val(array, item)) {
            return false;
        }
    }
    return yyjson_mut_obj_add_val(doc, root, "candidates", array);
}

static bool add_candidate_evidence(cbm_store_t *store, yyjson_mut_doc *doc,
                                   yyjson_mut_val *evidence, const cbm_context_request_t *request,
                                   context_emit_state_t *state) {
    int selected_capacity = (int)(sizeof(state->selected) / sizeof(state->selected[0]));
    for (int i = 0; i < state->candidate_count && state->evidence_count < state->evidence_limit;
         i++) {
        cbm_node_t *node = &state->candidates[i];
        if ((!request->include_tests && is_test_node(node)) ||
            node_seen(state->selected, state->selected_count, node->id)) {
            continue;
        }
        if (state->selected_count >= selected_capacity) {
            break;
        }
        state->selected[state->selected_count++] = *node;
        if (!add_evidence_json(store, doc, evidence, node, &state->ranks[i], i + 1,
                               state->neighbor_limit, "graph_node_and_direct_neighbors")) {
            return false;
        }
        state->evidence_count++;
        state->estimated_tokens += CONTEXT_ESTIMATE_PER_NODE;
    }
    return true;
}

static bool add_related_test_evidence(cbm_store_t *store, yyjson_mut_doc *doc,
                                      yyjson_mut_val *evidence,
                                      const cbm_context_request_t *request,
                                      context_emit_state_t *state) {
    int selected_capacity = (int)(sizeof(state->selected) / sizeof(state->selected[0]));
    if (!request->include_tests || state->evidence_count >= state->evidence_limit) {
        return true;
    }
    for (int i = 0; i < state->candidate_count && state->evidence_count < state->evidence_limit;
         i++) {
        const char *name = state->candidates[i].name;
        if (!name || !name[0]) {
            continue;
        }
        char test_names[3][CBM_SZ_256];
        snprintf(test_names[0], sizeof(test_names[0]), "test_%s", name);
        snprintf(test_names[1], sizeof(test_names[1]), "%s_test", name);
        snprintf(test_names[2], sizeof(test_names[2]), "Test%s", name);
        for (int pattern = 0; pattern < 3 && state->evidence_count < state->evidence_limit;
             pattern++) {
            cbm_node_t *test_nodes = NULL;
            int test_count = 0;
            if (cbm_store_find_nodes_by_name(store, request->project, test_names[pattern],
                                             &test_nodes, &test_count) != CBM_STORE_OK) {
                continue;
            }
            for (int j = 0; j < test_count && state->evidence_count < state->evidence_limit; j++) {
                cbm_node_t *test = &test_nodes[j];
                if (!is_test_node(test) ||
                    node_seen(state->selected, state->selected_count, test->id)) {
                    continue;
                }
                if (state->selected_count >= selected_capacity) {
                    cbm_store_free_nodes(test_nodes, test_count);
                    return true;
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
                state->selected[state->selected_count++] = *test;
                if (!add_evidence_json(store, doc, evidence, test, &test_rank,
                                       state->candidate_count + state->evidence_count + 1,
                                       state->neighbor_limit,
                                       "related_test_and_direct_neighbors")) {
                    cbm_store_free_nodes(test_nodes, test_count);
                    return false;
                }
                state->evidence_count++;
                state->estimated_tokens += CONTEXT_ESTIMATE_PER_NODE;
            }
            cbm_store_free_nodes(test_nodes, test_count);
        }
    }
    return true;
}

static bool add_documentation_json(cbm_store_t *store, yyjson_mut_doc *doc, yyjson_mut_val *root,
                                   const cbm_context_request_t *request, int evidence_limit,
                                   bool *truncated) {
    yyjson_mut_val *documentation = yyjson_mut_arr(doc);
    if (!documentation) {
        return false;
    }
    *truncated = false;
    if (request->include_docs) {
        char **doc_paths = NULL;
        int doc_count = 0;
        if (cbm_store_find_architecture_docs(store, request->project, &doc_paths, &doc_count) ==
            CBM_STORE_OK) {
            int doc_limit = evidence_limit;
            *truncated = doc_count > doc_limit;
            bool ok = true;
            for (int i = 0; i < doc_count; i++) {
                if (i < doc_limit && doc_paths[i]) {
                    ok = ok && yyjson_mut_arr_add_strcpy(doc, documentation, doc_paths[i]);
                }
                free(doc_paths[i]);
            }
            free(doc_paths);
            if (!ok) {
                return false;
            }
        }
    }
    return yyjson_mut_obj_add_val(doc, root, "documentation", documentation);
}

static bool add_budget_json(yyjson_mut_doc *doc, yyjson_mut_val *root, int budget,
                            const context_emit_state_t *state, bool truncated) {
    yyjson_mut_val *budget_obj = yyjson_mut_obj(doc);
    return budget_obj && yyjson_mut_obj_add_int(doc, budget_obj, "requested_tokens", budget) &&
           yyjson_mut_obj_add_int(doc, budget_obj, "estimated_tokens", state->estimated_tokens) &&
           yyjson_mut_obj_add_int(doc, budget_obj, "evidence_limit", state->evidence_limit) &&
           yyjson_mut_obj_add_int(doc, budget_obj, "neighbor_limit", state->neighbor_limit) &&
           yyjson_mut_obj_add_bool(doc, budget_obj, "truncated", truncated) &&
           yyjson_mut_obj_add_val(doc, root, "budget", budget_obj);
}

static bool add_limitations_json(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                 const cbm_context_request_t *request, int candidate_count,
                                 bool budget_truncated, bool documentation_truncated) {
    yyjson_mut_val *limitations = yyjson_mut_arr(doc);
    bool ok = limitations &&
              yyjson_mut_arr_add_str(
                  doc, limitations,
                  "Graph evidence is deterministic but best-effort; dynamic calls and reflection "
                  "may be absent.");
    if (candidate_count == 0) {
        ok = ok && yyjson_mut_arr_add_str(
                       doc, limitations,
                       "No unique target was resolved; use search_graph to find an exact qualified "
                       "name.");
    } else if (candidate_count > 1) {
        ok = ok && yyjson_mut_arr_add_str(
                       doc, limitations,
                       "The target is ambiguous; choose one candidate before relying on impact "
                       "conclusions.");
    }
    if (!request->include_docs) {
        ok = ok &&
             yyjson_mut_arr_add_str(doc, limitations, "Documentation evidence was not requested.");
    }
    if (request->diff_ref && request->diff_ref[0]) {
        ok = ok && yyjson_mut_arr_add_str(
                       doc, limitations,
                       "The diff reference is recorded for this context; use detect_changes for "
                       "full Git diff evidence.");
    }
    if (budget_truncated) {
        ok = ok && yyjson_mut_arr_add_str(doc, limitations,
                                          "Evidence was trimmed to the requested token budget.");
    }
    if (documentation_truncated) {
        ok = ok && yyjson_mut_arr_add_str(
                       doc, limitations,
                       "Documentation evidence was trimmed to the requested token budget.");
    }
    return ok && yyjson_mut_obj_add_val(doc, root, "limitations", limitations);
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

    context_emit_state_t state = {
        .evidence_limit = evidence_limit,
        .neighbor_limit = neighbor_limit,
    };
    if (!prepare_candidates(store, request, &state)) {
        free_emit_state(&state);
        return context_strdup("{\"error\":\"unable to allocate context ranking\"}");
    }
    int candidate_budget_limit = evidence_limit > 0 ? evidence_limit : 1;
    if (candidate_budget_limit > CONTEXT_DEFAULT_LIMIT) {
        candidate_budget_limit = CONTEXT_DEFAULT_LIMIT;
    }
    int candidate_limit = state.candidate_count < candidate_budget_limit ? state.candidate_count
                                                                         : candidate_budget_limit;
    /* Even with a tiny budget, preserve two ambiguous candidates so callers
     * can disambiguate instead of receiving an arbitrary single choice. */
    if (state.candidate_count > 1 && candidate_limit < 2) {
        candidate_limit = 2;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        free_emit_state(&state);
        return context_strdup("{\"error\":\"out of memory\"}");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        goto json_oom;
    }
    yyjson_mut_doc_set_root(doc, root);
    bool json_ok = yyjson_mut_obj_add_strcpy(doc, root, "project", request->project);
    json_ok = json_ok && yyjson_mut_obj_add_strcpy(doc, root, "task", request->task);
    json_ok = json_ok && add_nullable_string(doc, root, "target", request->target);
    json_ok = json_ok && add_nullable_string(doc, root, "diff_ref", request->diff_ref);
    json_ok = json_ok &&
              add_nullable_string(doc, root, "evidence_level",
                                  request->evidence_level ? request->evidence_level : "analysis");
    if (!json_ok) {
        goto json_oom;
    }

    if (!add_resolved_target_json(store, doc, root, &state) ||
        !add_candidates_json(store, doc, root, request, &state, candidate_limit)) {
        goto json_oom;
    }

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    if (!evidence || !add_candidate_evidence(store, doc, evidence, request, &state) ||
        !add_related_test_evidence(store, doc, evidence, request, &state) ||
        !yyjson_mut_obj_add_val(doc, root, "evidence", evidence)) {
        goto json_oom;
    }

    bool documentation_truncated = false;
    if (!add_documentation_json(store, doc, root, request, evidence_limit,
                                &documentation_truncated)) {
        goto json_oom;
    }

    bool budget_truncated = state.candidate_count > candidate_limit ||
                            state.evidence_count < state.candidate_count || documentation_truncated;
    bool resolution_incomplete = state.candidate_count != 1;
    if (!add_budget_json(doc, root, budget, &state, budget_truncated) ||
        !add_limitations_json(doc, root, request, state.candidate_count, budget_truncated,
                              documentation_truncated)) {
        goto json_oom;
    }

    size_t length = 0;
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &length);
    if (!json) {
        goto json_oom;
    }
    yyjson_mut_doc_free(doc);
    free_emit_state(&state);
    if (stats) {
        stats->returned = state.evidence_count;
        stats->total = state.candidate_count;
        stats->estimated_tokens = state.estimated_tokens;
        stats->budget_truncated = budget_truncated;
        stats->resolution_incomplete = resolution_incomplete;
        stats->truncated = budget_truncated || resolution_incomplete;
    }
    return json;

json_oom:
    yyjson_mut_doc_free(doc);
    free_emit_state(&state);
    return context_strdup("{\"error\":\"out of memory\"}");
}
