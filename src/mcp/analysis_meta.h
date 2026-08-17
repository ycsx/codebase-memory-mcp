#ifndef CBM_ANALYSIS_META_H
#define CBM_ANALYSIS_META_H

#include "git/git_context.h"
#include "store/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson/yyjson.h>

typedef struct {
    const char *tool;
    const char *profile;
    const char *project;
    const cbm_project_t *project_info;
    const cbm_project_metadata_t *graph_meta;
    const cbm_git_context_t *source;
    const cbm_coverage_meta_t *coverage_meta;
    bool have_coverage;
    int known_gap_count;
    int excluded_count;
    bool coverage_details_truncated;
    const char *result_status;
    bool result_truncated;
    int64_t returned;
    int64_t total;
    int64_t limit;
    int has_more; /* -1 unknown, 0 false, 1 true */
    const char *next_cursor;
    const char *const *truncation_reasons;
    size_t truncation_reason_count;
} cbm_analysis_meta_input_t;

/* Build and attach the canonical analysis_meta object. */
bool cbm_analysis_meta_add(yyjson_mut_doc *doc, yyjson_mut_val *parent,
                           const cbm_analysis_meta_input_t *input);

#endif
