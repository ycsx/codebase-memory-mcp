#include "mcp/analysis_meta.h"

#include "foundation/compat.h"
#include "foundation/constants.h"

#include <string.h>
#include <time.h>

typedef struct {
    const char *status;
    const char *basis;
    const char *reasons[3];
    size_t reason_count;
    bool blocking;
} freshness_result_t;

typedef struct {
    const char *status;
    int generation_match;
} coverage_result_t;

static bool has_text(const char *value) {
    return value && value[0];
}

static void add_nullable_string(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                const char *value) {
    if (has_text(value)) {
        yyjson_mut_obj_add_strcpy(doc, obj, key, value);
    } else {
        yyjson_mut_obj_add_null(doc, obj, key);
    }
}

static void add_nullable_int(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                             int64_t value) {
    if (value >= 0) {
        yyjson_mut_obj_add_sint(doc, obj, key, value);
    } else {
        yyjson_mut_obj_add_null(doc, obj, key);
    }
}

static void add_reason_array(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                             const char *const *reasons, size_t count) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (!reasons) {
        count = 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (has_text(reasons[i])) {
            yyjson_mut_arr_add_strcpy(doc, arr, reasons[i]);
        }
    }
    yyjson_mut_obj_add_val(doc, obj, key, arr);
}

static freshness_result_t evaluate_freshness(const cbm_analysis_meta_input_t *input) {
    freshness_result_t out = {
        .status = "unknown",
        .basis = "unavailable",
        .blocking = true,
    };
    const cbm_project_metadata_t *graph = input->graph_meta;
    const cbm_git_context_t *source = input->source;
    if (!graph || !has_text(graph->generation_id)) {
        out.reasons[out.reason_count++] = "metadata_missing";
        return out;
    }
    if (!source) {
        out.reasons[out.reason_count++] = "source_unavailable";
        return out;
    }
    if (!source->root_exists) {
        out.reasons[out.reason_count++] = "source_missing";
        return out;
    }
    if (!source->is_git) {
        out.reasons[out.reason_count++] = "no_git";
        out.blocking = false;
        return out;
    }

    out.basis = "git";
    if (!has_text(graph->indexed_commit) || !has_text(source->head_sha)) {
        out.reasons[out.reason_count++] = "metadata_missing";
        return out;
    }
    bool commit_mismatch = strcmp(graph->indexed_commit, source->head_sha) != 0;
    bool dirty = source->worktree_state == CBM_GIT_WORKTREE_DIRTY;
    if (commit_mismatch || dirty) {
        out.status = "stale";
        out.blocking = false;
        if (commit_mismatch) {
            out.reasons[out.reason_count++] = "commit_mismatch";
        }
        if (dirty) {
            out.reasons[out.reason_count++] = "worktree_dirty";
        }
        return out;
    }
    if (source->worktree_state != CBM_GIT_WORKTREE_CLEAN) {
        out.reasons[out.reason_count++] = "source_unavailable";
        return out;
    }
    out.status = "current";
    out.blocking = false;
    return out;
}

static coverage_result_t evaluate_coverage(const cbm_analysis_meta_input_t *input) {
    coverage_result_t out = {.status = "unknown", .generation_match = -1};
    const cbm_project_metadata_t *graph = input->graph_meta;
    const cbm_coverage_meta_t *coverage = input->coverage_meta;
    if (!input->have_coverage || !coverage || !graph || !has_text(graph->generation_id) ||
        !has_text(coverage->generation_id)) {
        return out;
    }
    out.generation_match = strcmp(graph->generation_id, coverage->generation_id) == 0 ? 1 : 0;
    if (out.generation_match == 0 || !coverage->hash_records_complete ||
        strcmp(coverage->recording_status ? coverage->recording_status : "", "complete") != 0 ||
        input->coverage_details_truncated) {
        return out;
    }
    out.status = input->known_gap_count > 0 || input->excluded_count > 0 ? "partial"
                                                                         : "complete_no_known_gap";
    return out;
}

static void add_limitation(yyjson_mut_doc *doc, yyjson_mut_val *limitations, const char *code,
                           const char *severity, const char *message, const char *scope,
                           const char *fallback) {
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, item, "code", code);
    yyjson_mut_obj_add_strcpy(doc, item, "severity", severity);
    yyjson_mut_obj_add_strcpy(doc, item, "message", message);
    yyjson_mut_val *scopes = yyjson_mut_arr(doc);
    if (has_text(scope)) {
        yyjson_mut_arr_add_strcpy(doc, scopes, scope);
    }
    yyjson_mut_obj_add_val(doc, item, "scopes", scopes);
    add_nullable_string(doc, item, "fallback_action", fallback);
    yyjson_mut_arr_add_val(limitations, item);
}

static const char *normalize_result_status(const cbm_analysis_meta_input_t *input) {
    if (input->result_truncated || input->has_more == 1) {
        return "partial";
    }
    if (input->result_status && (strcmp(input->result_status, "complete") == 0 ||
                                 strcmp(input->result_status, "partial") == 0 ||
                                 strcmp(input->result_status, "unknown") == 0)) {
        return input->result_status;
    }
    return "unknown";
}

static const char *normalize_profile(const char *profile) {
    if (profile && (strcmp(profile, "all") == 0 || strcmp(profile, "analysis") == 0 ||
                    strcmp(profile, "scout") == 0)) {
        return profile;
    }
    return "unknown";
}

static const char *normalize_recording_status(const cbm_coverage_meta_t *coverage) {
    const char *status = coverage ? coverage->recording_status : NULL;
    if (status && (strcmp(status, "complete") == 0 || strcmp(status, "truncated") == 0 ||
                   strcmp(status, "unavailable") == 0)) {
        return status;
    }
    return "unknown";
}

bool cbm_analysis_meta_add(yyjson_mut_doc *doc, yyjson_mut_val *parent,
                           const cbm_analysis_meta_input_t *input) {
    if (!doc || !parent || !input || !has_text(input->tool)) {
        return false;
    }
    freshness_result_t freshness = evaluate_freshness(input);
    coverage_result_t coverage = evaluate_coverage(input);
    const char *result_status = normalize_result_status(input);
    bool result_truncated = input->result_truncated || input->has_more == 1;
    int has_more = input->has_more;
    const char *next_cursor = input->next_cursor;
    if (has_more == 1 && !has_text(next_cursor)) {
        has_more = -1;
    }
    if (has_more != 1) {
        next_cursor = NULL;
    }

    char checked_at[CBM_SZ_64] = "";
    time_t now = time(NULL);
    struct tm utc;
    cbm_gmtime_r(&now, &utc);
    (void)strftime(checked_at, sizeof(checked_at), "%Y-%m-%dT%H:%M:%SZ", &utc);

    yyjson_mut_val *meta = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, meta, "schema_version", 1);
    yyjson_mut_obj_add_strcpy(doc, meta, "tool", input->tool);
    yyjson_mut_obj_add_strcpy(doc, meta, "profile", normalize_profile(input->profile));
    add_nullable_string(doc, meta, "project", input->project);

    yyjson_mut_val *graph = yyjson_mut_obj(doc);
    add_nullable_string(doc, graph, "generation_id",
                        input->graph_meta ? input->graph_meta->generation_id : NULL);
    add_nullable_string(doc, graph, "indexed_at",
                        input->project_info ? input->project_info->indexed_at : NULL);
    add_nullable_string(doc, graph, "generated_at",
                        input->graph_meta ? input->graph_meta->generated_at : NULL);
    yyjson_mut_obj_add_strcpy(doc, graph, "index_mode",
                              input->coverage_meta && has_text(input->coverage_meta->index_mode)
                                  ? input->coverage_meta->index_mode
                                  : "unknown");
    add_nullable_string(doc, graph, "indexed_commit",
                        input->graph_meta ? input->graph_meta->indexed_commit : NULL);
    yyjson_mut_obj_add_val(doc, meta, "graph", graph);

    yyjson_mut_val *source = yyjson_mut_obj(doc);
    const char *vcs = !input->source || !input->source->root_exists ? "unknown"
                      : input->source->is_git                       ? "git"
                                                                    : "none";
    yyjson_mut_obj_add_strcpy(doc, source, "vcs", vcs);
    add_nullable_string(doc, source, "current_commit",
                        input->source && input->source->is_git ? input->source->head_sha : NULL);
    add_nullable_string(doc, source, "branch",
                        input->source && input->source->is_git ? input->source->branch : NULL);
    const char *worktree_state = "unknown";
    if (input->source && input->source->root_exists && !input->source->is_git) {
        worktree_state = "not_applicable";
    } else if (input->source && input->source->worktree_state == CBM_GIT_WORKTREE_CLEAN) {
        worktree_state = "clean";
    } else if (input->source && input->source->worktree_state == CBM_GIT_WORKTREE_DIRTY) {
        worktree_state = "dirty";
    }
    yyjson_mut_obj_add_strcpy(doc, source, "worktree_state", worktree_state);
    yyjson_mut_obj_add_val(doc, meta, "source", source);

    yyjson_mut_val *freshness_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, freshness_obj, "status", freshness.status);
    yyjson_mut_obj_add_strcpy(doc, freshness_obj, "basis", freshness.basis);
    add_nullable_string(doc, freshness_obj, "checked_at", checked_at);
    add_reason_array(doc, freshness_obj, "reason_codes", freshness.reasons, freshness.reason_count);
    yyjson_mut_obj_add_val(doc, meta, "freshness", freshness_obj);

    yyjson_mut_val *coverage_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, coverage_obj, "status", coverage.status);
    yyjson_mut_obj_add_str(doc, coverage_obj, "signal", "best_effort");
    yyjson_mut_obj_add_strcpy(doc, coverage_obj, "recording_status",
                              normalize_recording_status(input->coverage_meta));
    add_nullable_string(doc, coverage_obj, "generation_id",
                        input->coverage_meta ? input->coverage_meta->generation_id : NULL);
    if (coverage.generation_match < 0) {
        yyjson_mut_obj_add_null(doc, coverage_obj, "generation_match");
    } else {
        yyjson_mut_obj_add_bool(doc, coverage_obj, "generation_match",
                                coverage.generation_match == 1);
    }
    bool coverage_counts_known = strcmp(coverage.status, "unknown") != 0;
    add_nullable_int(doc, coverage_obj, "known_gap_count",
                     coverage_counts_known ? input->known_gap_count : -1);
    add_nullable_int(doc, coverage_obj, "excluded_count",
                     coverage_counts_known ? input->excluded_count : -1);
    if (input->have_coverage && input->coverage_meta) {
        yyjson_mut_obj_add_bool(doc, coverage_obj, "hash_records_complete",
                                input->coverage_meta->hash_records_complete);
    } else {
        yyjson_mut_obj_add_null(doc, coverage_obj, "hash_records_complete");
    }
    yyjson_mut_obj_add_bool(doc, coverage_obj, "details_truncated",
                            input->coverage_details_truncated);
    yyjson_mut_obj_add_val(doc, meta, "coverage", coverage_obj);

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, result, "status", result_status);
    yyjson_mut_obj_add_bool(doc, result, "truncated", result_truncated);
    add_nullable_int(doc, result, "returned", input->returned);
    add_nullable_int(doc, result, "total", input->total);
    add_nullable_int(doc, result, "limit", input->limit);
    if (has_more < 0) {
        yyjson_mut_obj_add_null(doc, result, "has_more");
    } else {
        yyjson_mut_obj_add_bool(doc, result, "has_more", has_more == 1);
    }
    add_nullable_string(doc, result, "next_cursor", next_cursor);
    if (result_truncated && (!input->truncation_reasons || input->truncation_reason_count == 0)) {
        const char *unknown_reason[] = {"unknown"};
        add_reason_array(doc, result, "truncation_reasons", unknown_reason, 1);
    } else {
        add_reason_array(doc, result, "truncation_reasons", input->truncation_reasons,
                         input->truncation_reason_count);
    }
    yyjson_mut_obj_add_val(doc, meta, "result", result);

    bool freshness_unknown = strcmp(freshness.status, "unknown") == 0;
    bool coverage_unknown = strcmp(coverage.status, "unknown") == 0;
    bool result_unknown = strcmp(result_status, "unknown") == 0;
    const char *confidence_level =
        freshness_unknown || coverage_unknown || result_unknown ? "unknown" : "best_effort";
    const char *confidence_reason = strcmp(confidence_level, "unknown") == 0
                                        ? "required_evidence_unavailable"
                                        : "static_graph_evidence";
    yyjson_mut_val *confidence = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, confidence, "level", confidence_level);
    const char *confidence_reasons[] = {confidence_reason};
    add_reason_array(doc, confidence, "reasons", confidence_reasons, 1);
    yyjson_mut_obj_add_val(doc, meta, "confidence", confidence);

    bool provisional = strcmp(freshness.status, "current") != 0 ||
                       strcmp(coverage.status, "partial") == 0 ||
                       strcmp(result_status, "partial") == 0;
    const char *claim_status = freshness.blocking || coverage_unknown || result_unknown
                                   ? "insufficient"
                               : provisional ? "provisional"
                                             : "supported";
    const char *claim_reason =
        strcmp(claim_status, "supported") == 0     ? "static_analysis_best_effort"
        : strcmp(claim_status, "provisional") == 0 ? "evidence_requires_qualification"
                                                   : "required_evidence_unavailable";
    yyjson_mut_val *claim = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, claim, "status", claim_status);
    yyjson_mut_obj_add_bool(doc, claim, "negative_claim_allowed", false);
    const char *claim_reasons[] = {claim_reason};
    add_reason_array(doc, claim, "reason_codes", claim_reasons, 1);
    yyjson_mut_obj_add_val(doc, meta, "claim", claim);

    yyjson_mut_val *limitations = yyjson_mut_arr(doc);
    add_limitation(
        doc, limitations, "static_analysis_best_effort", "info",
        "Dynamic calls, reflection, and runtime registration may be absent from the graph.",
        input->project, NULL);
    if (freshness.blocking) {
        add_limitation(doc, limitations, "freshness_unknown", "blocking",
                       "The current source could not be correlated with this graph generation.",
                       input->project, "reindex_project");
    } else if (strcmp(freshness.status, "stale") == 0) {
        add_limitation(doc, limitations, "stale_graph", "warning",
                       "The graph does not represent the current commit or dirty worktree.",
                       input->project, "reindex_project");
    }
    if (coverage_unknown) {
        add_limitation(doc, limitations, "coverage_unknown", "blocking",
                       "Coverage metadata cannot be trusted for this graph generation.",
                       input->project, "reindex_project");
    } else if (strcmp(coverage.status, "partial") == 0) {
        add_limitation(doc, limitations, "known_coverage_gaps", "warning",
                       "Recorded parse, extraction, or exclusion gaps intersect the analysis.",
                       input->project, "read_flagged_source");
    }
    if (strcmp(result_status, "partial") == 0) {
        add_limitation(doc, limitations, "result_incomplete", "warning",
                       "The response is truncated or has an unconsumed continuation.",
                       input->project, has_more == 1 ? "continue_with_cursor" : NULL);
    }
    yyjson_mut_obj_add_val(doc, meta, "limitations", limitations);

    yyjson_mut_obj_add_val(doc, parent, "analysis_meta", meta);
    return true;
}
