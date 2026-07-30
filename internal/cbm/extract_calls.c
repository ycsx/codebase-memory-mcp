#include "cbm.h"
#include "arena.h" // CBMArena, cbm_arena_sprintf
#include "helpers.h"
#include "lang_specs.h"
#include "macro_table.h"
#include "extract_unified.h"
#include "foundation/constants.h"
#include "extract_node_stack.h"
#include "service_patterns.h" // cbm_service_pattern_route_method (#952)
#include "tree_sitter/api.h"  // TSNode, ts_node_*
#include <stdint.h>           // uint32_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Max ancestor depth for Lean type-position check. */
enum { LEAN_MAX_PARENT_DEPTH = 20 };
/* Max positional args to scan for URL/string. */
enum { MAX_POSITIONAL_SCAN = 3 };
/* Max positional args to scan for handler ref. */
enum { MAX_HANDLER_SCAN = 4 };
/* Max string arg length before rejection. */
enum { MAX_STRING_ARG_LEN = CBM_SZ_512 };
/* Min printable ASCII (space). */
enum { MIN_PRINTABLE = 0x20 };
/* Handler arg scan start index (skip first positional). */
enum { HANDLER_START_IDX = 1 };

/* Look up a module-level string constant by name. */
static const char *lookup_string_constant(const CBMExtractCtx *ctx, const char *name) {
    if (!name || !name[0]) {
        return NULL;
    }
    const CBMStringConstantMap *map = &ctx->string_constants;
    /* Later assignments shadow earlier ones. The map is populated in source
     * order by the
     * unified walk, so scan backwards. */
    for (int i = map->count - 1; i >= 0; i--) {
        if (strcmp(map->names[i], name) == 0) {
            return map->values[i];
        }
    }
    return NULL;
}

/* Check if a node type is a string literal */
static int is_string_like(const char *kind) {
    return (strcmp(kind, "string") == 0 || strcmp(kind, "string_literal") == 0 ||
            strcmp(kind, "interpreted_string_literal") == 0 ||
            strcmp(kind, "raw_string_literal") == 0 || strcmp(kind, "string_content") == 0);
}

/* Strip surrounding quotes from a string, return arena-allocated copy */
static const char *strip_quotes(CBMArena *a, const char *text) {
    if (!text || !text[0]) {
        return NULL;
    }
    int len = (int)strlen(text);
    if (len >= CBM_QUOTE_PAIR && (text[0] == '"' || text[0] == '\'')) {
        return cbm_arena_strndup(a, text + CBM_QUOTE_OFFSET, (size_t)(len - CBM_QUOTE_PAIR));
    }
    return text;
}

static TSNode python_enclosing_scope(TSNode node) {
    TSNode cur = node;
    while (!ts_node_is_null(cur)) {
        const char *kind = ts_node_type(cur);
        if (strcmp(kind, "function_definition") == 0 || strcmp(kind, "lambda") == 0 ||
            strcmp(kind, "module") == 0) {
            return cur;
        }
        cur = ts_node_parent(cur);
    }
    return (TSNode){0};
}

/* Find the most recent assignment to a Python local before the argument use.
 * This deliberately
 * stays intra-function: a same-named local in another
 * function must never leak into URL
 * propagation. */
static TSNode python_prior_assignment_value(CBMExtractCtx *ctx, TSNode use_node, const char *name) {
    if (ctx->language != CBM_LANG_PYTHON || !name || !name[0]) {
        return (TSNode){0};
    }
    TSNode use_scope = python_enclosing_scope(use_node);
    if (ts_node_is_null(use_scope)) {
        return (TSNode){0};
    }
    uint32_t use_start = ts_node_start_byte(use_node);
    uint32_t best_start = 0;
    TSNode best = {0};
    TSTreeCursor cursor = ts_tree_cursor_new(use_scope);
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        uint32_t start = ts_node_start_byte(node);
        if (start < use_start && strcmp(ts_node_type(node), "assignment") == 0) {
            TSNode left = ts_node_child_by_field_name(node, TS_FIELD("left"));
            TSNode right = ts_node_child_by_field_name(node, TS_FIELD("right"));
            if (!ts_node_is_null(left) && !ts_node_is_null(right) &&
                strcmp(ts_node_type(left), "identifier") == 0) {
                char *lhs = cbm_node_text(ctx->arena, left, ctx->source);
                if (lhs && strcmp(lhs, name) == 0 && start >= best_start) {
                    best_start = start;
                    best = right;
                }
            }
        }
        if (start >= use_start) {
            /* No descendant can precede the use once this subtree starts at
             * or after
             * it. Skip descending but keep scanning siblings. */
        } else if (ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            continue;
        }
        bool advanced = false;
        while (ts_tree_cursor_goto_parent(&cursor)) {
            if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                advanced = true;
                break;
            }
        }
        if (!advanced) {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    return best;
}

static bool is_js_language(CBMLanguage language) {
    return language == CBM_LANG_JAVASCRIPT || language == CBM_LANG_TYPESCRIPT ||
           language == CBM_LANG_TSX;
}

static bool is_js_scope_kind(const char *kind) {
    return strcmp(kind, "function_declaration") == 0 || strcmp(kind, "function_expression") == 0 ||
           strcmp(kind, "arrow_function") == 0 || strcmp(kind, "method_definition") == 0 ||
           strcmp(kind, "program") == 0;
}

static TSNode js_enclosing_scope(TSNode node) {
    TSNode cur = node;
    while (!ts_node_is_null(cur)) {
        const char *kind = ts_node_type(cur);
        if (is_js_scope_kind(kind)) {
            return cur;
        }
        cur = ts_node_parent(cur);
    }
    return (TSNode){0};
}

/* Resolve both `const url = ...` and later `this.pageSet = ...` writes. Full
 * LHS text is
 * compared so member chains can be followed without pretending to
 * understand arbitrary
 * JavaScript aliasing. */
static TSNode js_prior_assignment_value(CBMExtractCtx *ctx, TSNode use_node, const char *name) {
    if (!is_js_language(ctx->language) || !name || !name[0]) {
        return (TSNode){0};
    }
    TSNode use_scope = js_enclosing_scope(use_node);
    if (ts_node_is_null(use_scope)) {
        return (TSNode){0};
    }
    uint32_t use_start = ts_node_start_byte(use_node);
    uint32_t best_start = 0;
    TSNode best = {0};
    TSTreeCursor cursor = ts_tree_cursor_new(use_scope);
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        uint32_t start = ts_node_start_byte(node);
        const char *kind = ts_node_type(node);
        TSNode left = {0};
        TSNode right = {0};
        if (start < use_start && strcmp(kind, "variable_declarator") == 0) {
            left = ts_node_child_by_field_name(node, TS_FIELD("name"));
            right = ts_node_child_by_field_name(node, TS_FIELD("value"));
        } else if (start < use_start && strcmp(kind, "assignment_expression") == 0) {
            left = ts_node_child_by_field_name(node, TS_FIELD("left"));
            right = ts_node_child_by_field_name(node, TS_FIELD("right"));
        }
        if (!ts_node_is_null(left) && !ts_node_is_null(right) &&
            ts_node_end_byte(right) <= use_start) {
            char *lhs = cbm_node_text(ctx->arena, left, ctx->source);
            if (lhs && strcmp(lhs, name) == 0 && start >= best_start) {
                best_start = start;
                best = right;
            }
        }
        bool nested_scope = !ts_node_eq(node, use_scope) && is_js_scope_kind(kind);
        if (start < use_start && !nested_scope && ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            continue;
        }
        bool advanced = false;
        while (ts_tree_cursor_goto_parent(&cursor)) {
            if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                advanced = true;
                break;
            }
        }
        if (!advanced) {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    return best;
}

const char *cbm_evaluate_static_string(CBMExtractCtx *ctx, TSNode node, int depth);

static bool append_string_piece(char *buf, size_t cap, size_t *len, const char *piece) {
    size_t n = piece ? strlen(piece) : 0;
    if (!piece || *len + n >= cap) {
        return false;
    }
    memcpy(buf + *len, piece, n);
    *len += n;
    buf[*len] = '\0';
    return true;
}

/* Decode a Python string/f-string sufficiently for route identity. Unknown
 * interpolation
 * expressions become ':' placeholders, matching route
 * canonicalization rather than discarding an
 * otherwise static API path. */
static const char *evaluate_python_string_text(CBMExtractCtx *ctx, TSNode node, const char *raw,
                                               int depth) {
    const char *q = raw;
    bool formatted = false;
    while (*q && *q != '\'' && *q != '"') {
        if (*q == 'f' || *q == 'F') {
            formatted = true;
        } else if (*q != 'r' && *q != 'R' && *q != 'u' && *q != 'U' && *q != 'b' && *q != 'B') {
            return NULL;
        }
        q++;
    }
    if (!*q) {
        return NULL;
    }
    char quote = *q;
    int qlen = (q[1] == quote && q[2] == quote) ? 3 : 1;
    const char *body = q + qlen;
    size_t raw_len = strlen(raw);
    if (raw_len < (size_t)(body - raw) + (size_t)qlen) {
        return NULL;
    }
    const char *end = raw + raw_len - qlen;
    if (end < body) {
        return NULL;
    }
    char out[MAX_STRING_ARG_LEN];
    size_t oi = 0;
    for (const char *p = body; p < end && oi + 1 < sizeof(out);) {
        if (formatted && *p == '{' && p + 1 < end && p[1] != '{') {
            const char *close = p + 1;
            while (close < end && *close != '}') {
                close++;
            }
            if (close >= end) {
                return NULL;
            }
            const char *value = NULL;
            size_t expr_len = (size_t)(close - (p + 1));
            if (expr_len > 0 && expr_len < CBM_SZ_256) {
                char expr[CBM_SZ_256];
                memcpy(expr, p + 1, expr_len);
                expr[expr_len] = '\0';
                char *bang = strpbrk(expr, "!:");
                if (bang) {
                    *bang = '\0';
                }
                value = lookup_string_constant(ctx, expr);
                if (!value && depth < 8) {
                    TSNode assigned = python_prior_assignment_value(ctx, node, expr);
                    if (!ts_node_is_null(assigned)) {
                        value = cbm_evaluate_static_string(ctx, assigned, depth + 1);
                    }
                }
            }
            /* A leading unknown followed by '/' is normally a configurable
             * authority
             * (for example f"{JAVA_URL}/api/..."); omit it and keep
             * the route path.
             * Unknowns inside the path are canonical route
             * parameters and must use
             * "{}", which cbm_route_canon_path
             * recognizes even when the source
             * parameter name is unavailable. */
            bool leading_authority = !value && oi == 0 && close + 1 < end && close[1] == '/';
            const char *piece = value ? value : (leading_authority ? "" : "{}");
            if (!append_string_piece(out, sizeof(out), &oi, piece)) {
                return NULL;
            }
            p = close + 1;
        } else if (formatted && p + 1 < end &&
                   ((*p == '{' && p[1] == '{') || (*p == '}' && p[1] == '}'))) {
            out[oi++] = *p;
            p += 2;
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
    return oi ? cbm_arena_strndup(ctx->arena, out, oi) : NULL;
}

static const char *resolve_js_template_expression(CBMExtractCtx *ctx, TSNode node,
                                                  const char *expr_start, const char *expr_end,
                                                  int depth) {
    while (expr_start < expr_end && isspace((unsigned char)*expr_start)) {
        expr_start++;
    }
    size_t expr_len = (size_t)(expr_end - expr_start);
    while (expr_len != 0 && isspace((unsigned char)expr_start[expr_len - 1])) {
        expr_len--;
    }
    if (expr_len == 0 || expr_len >= CBM_SZ_256) {
        return NULL;
    }

    char expr[CBM_SZ_256];
    memcpy(expr, expr_start, expr_len);
    expr[expr_len] = '\0';
    const char *value = lookup_string_constant(ctx, expr);
    if (!value && depth < 8) {
        TSNode assigned = js_prior_assignment_value(ctx, node, expr);
        if (!ts_node_is_null(assigned)) {
            value = cbm_evaluate_static_string(ctx, assigned, depth + 1);
        }
    }
    return value;
}

static const char *evaluate_js_template_text(CBMExtractCtx *ctx, TSNode node, const char *raw,
                                             int depth) {
    size_t raw_len = raw ? strlen(raw) : 0;
    if (raw_len < 2 || raw[0] != '`' || raw[raw_len - 1] != '`') {
        return NULL;
    }
    const char *end = raw + raw_len - 1;
    char out[MAX_STRING_ARG_LEN];
    size_t oi = 0;
    for (const char *p = raw + 1; p < end && oi + 1 < sizeof(out);) {
        if (*p == '$' && p + 1 < end && p[1] == '{') {
            const char *close = p + 2;
            int braces = 1;
            while (close < end && braces > 0) {
                if (*close == '{') {
                    braces++;
                } else if (*close == '}') {
                    braces--;
                }
                if (braces > 0) {
                    close++;
                }
            }
            if (close >= end || braces != 0) {
                return NULL;
            }
            const char *value = resolve_js_template_expression(ctx, node, p + 2, close, depth);
            bool leading_authority = !value && oi == 0 && close + 1 < end && close[1] == '/';
            const char *piece = value ? value : (leading_authority ? "" : "{}");
            if (!append_string_piece(out, sizeof(out), &oi, piece)) {
                return NULL;
            }
            p = close + 1;
            continue;
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    return oi ? cbm_arena_strndup(ctx->arena, out, oi) : NULL;
}

const char *cbm_evaluate_static_string(CBMExtractCtx *ctx, TSNode node, int depth) {
    if (ts_node_is_null(node) || depth >= 8) {
        return NULL;
    }
    const char *kind = ts_node_type(node);
    if (strcmp(kind, "identifier") == 0) {
        char *name = cbm_node_text(ctx->arena, node, ctx->source);
        if (is_js_language(ctx->language)) {
            TSNode assigned = js_prior_assignment_value(ctx, node, name);
            return ts_node_is_null(assigned) ? NULL
                                             : cbm_evaluate_static_string(ctx, assigned, depth + 1);
        }
        const char *known = lookup_string_constant(ctx, name);
        if (known) {
            return known;
        }
        TSNode assigned = ctx->language == CBM_LANG_PYTHON
                              ? python_prior_assignment_value(ctx, node, name)
                              : js_prior_assignment_value(ctx, node, name);
        return ts_node_is_null(assigned) ? NULL
                                         : cbm_evaluate_static_string(ctx, assigned, depth + 1);
    }
    if (strcmp(kind, "parenthesized_expression") == 0 && ts_node_named_child_count(node) == 1) {
        return cbm_evaluate_static_string(ctx, ts_node_named_child(node, 0), depth + 1);
    }
    if (strcmp(kind, "binary_operator") == 0 || strcmp(kind, "binary_expression") == 0) {
        TSNode left = ts_node_child_by_field_name(node, TS_FIELD("left"));
        TSNode right = ts_node_child_by_field_name(node, TS_FIELD("right"));
        const char *lv = cbm_evaluate_static_string(ctx, left, depth + 1);
        const char *rv = cbm_evaluate_static_string(ctx, right, depth + 1);
        if (lv && rv) {
            size_t total = strlen(lv) + strlen(rv);
            if (total < MAX_STRING_ARG_LEN) {
                return cbm_arena_sprintf(ctx->arena, "%s%s", lv, rv);
            }
        }
        /* Config-driven hosts are often not statically knowable, but the
         * right-hand API
         * path is. Keep that suffix for route matching. */
        if (!lv && rv && rv[0] == '/') {
            return rv;
        }
        return NULL;
    }
    if (strcmp(kind, "ternary_expression") == 0) {
        TSNode consequence = ts_node_child_by_field_name(node, TS_FIELD("consequence"));
        return cbm_evaluate_static_string(ctx, consequence, depth + 1);
    }

    bool js_template = strcmp(kind, "template_string") == 0 && is_js_language(ctx->language);
    bool python_string = is_string_like(kind) || strcmp(kind, "concatenated_string") == 0;
    bool js_member = strcmp(kind, "member_expression") == 0 && is_js_language(ctx->language);
    if (!js_template && !python_string && !js_member) {
        return NULL;
    }

    char *raw = cbm_node_text(ctx->arena, node, ctx->source);
    if (!raw || !raw[0]) {
        return NULL;
    }
    if (js_template) {
        return evaluate_js_template_text(ctx, node, raw, depth);
    }
    if (python_string) {
        return evaluate_python_string_text(ctx, node, raw, depth);
    }
    return lookup_string_constant(ctx, raw);
}

enum { MAX_STATIC_STRING_CANDIDATES = 16 };

typedef struct {
    const char *values[MAX_STATIC_STRING_CANDIDATES];
    int count;
} StaticStringCandidates;

static void static_candidate_add(StaticStringCandidates *out, const char *value) {
    if (!value || !value[0] || out->count >= MAX_STATIC_STRING_CANDIDATES) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        if (strcmp(out->values[i], value) == 0) {
            return;
        }
    }
    out->values[out->count++] = value;
}

static void collect_static_string_candidates(CBMExtractCtx *ctx, TSNode node, TSNode use_node,
                                             int depth, StaticStringCandidates *out);

static const char *js_property_key(CBMExtractCtx *ctx, TSNode pair) {
    TSNode key = ts_node_child_by_field_name(pair, TS_FIELD("key"));
    if (ts_node_is_null(key)) {
        return NULL;
    }
    char *text = cbm_node_text(ctx->arena, key, ctx->source);
    if (!text || !text[0]) {
        return NULL;
    }
    size_t len = strlen(text);
    if (len >= 2 && (text[0] == '\'' || text[0] == '"') && text[len - 1] == text[0]) {
        return cbm_arena_strndup(ctx->arena, text + 1, len - 2);
    }
    return text;
}

static TSNode js_follow_assignment(CBMExtractCtx *ctx, TSNode use_node, TSNode node) {
    const char *kind = ts_node_type(node);
    if (strcmp(kind, "identifier") != 0 && strcmp(kind, "member_expression") != 0) {
        return node;
    }
    char *name = cbm_node_text(ctx->arena, node, ctx->source);
    TSNode assigned = js_prior_assignment_value(ctx, use_node, name);
    return ts_node_is_null(assigned) ? node : assigned;
}

static void collect_js_object_property(CBMExtractCtx *ctx, TSNode object_node, const char *property,
                                       TSNode use_node, int depth, StaticStringCandidates *out);

static const char *js_static_index_key(CBMExtractCtx *ctx, TSNode index, TSNode use_node,
                                       int depth) {
    StaticStringCandidates keys = {0};
    collect_static_string_candidates(ctx, index, use_node, depth + 1, &keys);
    return keys.count == 1 ? keys.values[0] : NULL;
}

/* Select one or every property from an object map. A dynamic index such as
 * `urlData[type]`
 * intentionally yields every statically listed URL. When
 * leaf_property is non-NULL, selection
 * continues into that nested property,
 * covering `pageData[type].url` and its aliased
 * `this.pageSet.url` form. */
static void collect_js_selected_values(CBMExtractCtx *ctx, TSNode base, TSNode index,
                                       const char *leaf_property, TSNode use_node, int depth,
                                       StaticStringCandidates *out) {
    if (depth >= 8 || out->count >= MAX_STATIC_STRING_CANDIDATES) {
        return;
    }
    TSNode resolved = js_follow_assignment(ctx, use_node, base);
    const char *kind = ts_node_type(resolved);
    if (strcmp(kind, "parenthesized_expression") == 0 && ts_node_named_child_count(resolved) == 1) {
        collect_js_selected_values(ctx, ts_node_named_child(resolved, 0), index, leaf_property,
                                   use_node, depth + 1, out);
        return;
    }
    if (strcmp(kind, "ternary_expression") == 0) {
        collect_js_selected_values(ctx,
                                   ts_node_child_by_field_name(resolved, TS_FIELD("consequence")),
                                   index, leaf_property, use_node, depth + 1, out);
        collect_js_selected_values(ctx,
                                   ts_node_child_by_field_name(resolved, TS_FIELD("alternative")),
                                   index, leaf_property, use_node, depth + 1, out);
        return;
    }
    if (strcmp(kind, "object") != 0) {
        return;
    }
    const char *wanted = js_static_index_key(ctx, index, use_node, depth);
    uint32_t count = ts_node_named_child_count(resolved);
    for (uint32_t i = 0; i < count && out->count < MAX_STATIC_STRING_CANDIDATES; i++) {
        TSNode pair = ts_node_named_child(resolved, i);
        if (strcmp(ts_node_type(pair), "pair") != 0) {
            continue;
        }
        const char *key = js_property_key(ctx, pair);
        if (wanted && (!key || strcmp(key, wanted) != 0)) {
            continue;
        }
        TSNode value = ts_node_child_by_field_name(pair, TS_FIELD("value"));
        if (ts_node_is_null(value)) {
            continue;
        }
        if (leaf_property) {
            collect_js_object_property(ctx, value, leaf_property, use_node, depth + 1, out);
        } else {
            collect_static_string_candidates(ctx, value, use_node, depth + 1, out);
        }
    }
}

static void collect_js_object_property(CBMExtractCtx *ctx, TSNode object_node, const char *property,
                                       TSNode use_node, int depth, StaticStringCandidates *out) {
    if (ts_node_is_null(object_node) || !property || depth >= 8 ||
        out->count >= MAX_STATIC_STRING_CANDIDATES) {
        return;
    }
    const char *original_kind = ts_node_type(object_node);
    if (strcmp(original_kind, "identifier") == 0 ||
        strcmp(original_kind, "member_expression") == 0) {
        char *base_name = cbm_node_text(ctx->arena, object_node, ctx->source);
        if (base_name) {
            const char *compound = cbm_arena_sprintf(ctx->arena, "%s.%s", base_name, property);
            static_candidate_add(out, lookup_string_constant(ctx, compound));
        }
    }
    TSNode resolved = js_follow_assignment(ctx, use_node, object_node);
    if (!ts_node_eq(resolved, object_node)) {
        collect_js_object_property(ctx, resolved, property, use_node, depth + 1, out);
        return;
    }
    const char *kind = ts_node_type(resolved);
    if (strcmp(kind, "parenthesized_expression") == 0 && ts_node_named_child_count(resolved) == 1) {
        collect_js_object_property(ctx, ts_node_named_child(resolved, 0), property, use_node,
                                   depth + 1, out);
        return;
    }
    if (strcmp(kind, "ternary_expression") == 0) {
        collect_js_object_property(ctx,
                                   ts_node_child_by_field_name(resolved, TS_FIELD("consequence")),
                                   property, use_node, depth + 1, out);
        collect_js_object_property(ctx,
                                   ts_node_child_by_field_name(resolved, TS_FIELD("alternative")),
                                   property, use_node, depth + 1, out);
        return;
    }
    if (strcmp(kind, "subscript_expression") == 0) {
        TSNode base = ts_node_child_by_field_name(resolved, TS_FIELD("object"));
        TSNode index = ts_node_child_by_field_name(resolved, TS_FIELD("index"));
        collect_js_selected_values(ctx, base, index, property, use_node, depth + 1, out);
        return;
    }
    if (strcmp(kind, "object") != 0) {
        return;
    }
    uint32_t count = ts_node_named_child_count(resolved);
    for (uint32_t i = 0; i < count; i++) {
        TSNode pair = ts_node_named_child(resolved, i);
        if (strcmp(ts_node_type(pair), "pair") != 0) {
            continue;
        }
        const char *key = js_property_key(ctx, pair);
        if (!key || strcmp(key, property) != 0) {
            continue;
        }
        TSNode value = ts_node_child_by_field_name(pair, TS_FIELD("value"));
        collect_static_string_candidates(ctx, value, use_node, depth + 1, out);
    }
}

static void collect_static_string_candidates(CBMExtractCtx *ctx, TSNode node, TSNode use_node,
                                             int depth, StaticStringCandidates *out) {
    if (ts_node_is_null(node) || depth >= 8 || out->count >= MAX_STATIC_STRING_CANDIDATES) {
        return;
    }
    const char *kind = ts_node_type(node);
    if (strcmp(kind, "ternary_expression") == 0) {
        collect_static_string_candidates(ctx,
                                         ts_node_child_by_field_name(node, TS_FIELD("consequence")),
                                         use_node, depth + 1, out);
        collect_static_string_candidates(ctx,
                                         ts_node_child_by_field_name(node, TS_FIELD("alternative")),
                                         use_node, depth + 1, out);
        return;
    }
    if (strcmp(kind, "identifier") == 0 && is_js_language(ctx->language)) {
        char *name = cbm_node_text(ctx->arena, node, ctx->source);
        const char *known = lookup_string_constant(ctx, name);
        if (known) {
            static_candidate_add(out, known);
            return;
        }
        TSNode assigned = js_prior_assignment_value(ctx, use_node, name);
        if (!ts_node_is_null(assigned)) {
            collect_static_string_candidates(ctx, assigned, use_node, depth + 1, out);
        }
        return;
    }
    if (strcmp(kind, "member_expression") == 0 && is_js_language(ctx->language)) {
        char *raw = cbm_node_text(ctx->arena, node, ctx->source);
        const char *known = lookup_string_constant(ctx, raw);
        if (known) {
            static_candidate_add(out, known);
            return;
        }
        TSNode object = ts_node_child_by_field_name(node, TS_FIELD("object"));
        TSNode property_node = ts_node_child_by_field_name(node, TS_FIELD("property"));
        char *property = cbm_node_text(ctx->arena, property_node, ctx->source);
        collect_js_object_property(ctx, object, property, use_node, depth + 1, out);
        return;
    }
    if (strcmp(kind, "subscript_expression") == 0 && is_js_language(ctx->language)) {
        TSNode base = ts_node_child_by_field_name(node, TS_FIELD("object"));
        TSNode index = ts_node_child_by_field_name(node, TS_FIELD("index"));
        collect_js_selected_values(ctx, base, index, NULL, use_node, depth + 1, out);
        return;
    }
    if ((strcmp(kind, "binary_expression") == 0 || strcmp(kind, "binary_operator") == 0) &&
        is_js_language(ctx->language)) {
        StaticStringCandidates left = {0};
        StaticStringCandidates right = {0};
        collect_static_string_candidates(ctx, ts_node_child_by_field_name(node, TS_FIELD("left")),
                                         use_node, depth + 1, &left);
        collect_static_string_candidates(ctx, ts_node_child_by_field_name(node, TS_FIELD("right")),
                                         use_node, depth + 1, &right);
        for (int li = 0; li < left.count; li++) {
            for (int ri = 0; ri < right.count; ri++) {
                if (strlen(left.values[li]) + strlen(right.values[ri]) < MAX_STRING_ARG_LEN) {
                    static_candidate_add(out, cbm_arena_sprintf(ctx->arena, "%s%s", left.values[li],
                                                                right.values[ri]));
                }
            }
        }
        if (left.count == 0) {
            for (int ri = 0; ri < right.count; ri++) {
                if (right.values[ri][0] == '/') {
                    static_candidate_add(out, right.values[ri]);
                }
            }
        }
        return;
    }
    if (strcmp(kind, "parenthesized_expression") == 0 && ts_node_named_child_count(node) == 1) {
        collect_static_string_candidates(ctx, ts_node_named_child(node, 0), use_node, depth + 1,
                                         out);
        return;
    }
    static_candidate_add(out, cbm_evaluate_static_string(ctx, node, depth));
}

// Callee suffixes for IRIS Python interop string-dispatch. Kept at file scope
// (not inside the function) to satisfy cppcheck variableScope.
static const char *s_py_dispatch_suffixes[] = {".classMethodValue", ".classMethodVoid",
                                               ".classMethodBoolean", ".classMethodObject", NULL};

// Per-language callee-suffix dispatch table — returns a NULL-terminated list of
// method-name suffixes whose calls should be resolved by extracting class+method
// from the first two string arguments (e.g. IRIS Python interop). Kept here
// rather than in CBMLangSpec to avoid -Wmissing-field-initializers across 155
// language rows.
const char **cbm_string_dispatch_suffixes(CBMLanguage lang) {
    if (lang == CBM_LANG_PYTHON) {
        return s_py_dispatch_suffixes;
    }
    return NULL;
}

// Forward declarations
static char *extract_callee_name(CBMArena *a, TSNode node, const char *source, CBMLanguage lang);
static char *gotemplate_callee(CBMArena *a, TSNode node, const char *source);
static const char *strip_and_validate_string_arg(CBMArena *a, char *text);

// Lean 4: check if an apply node is inside a type annotation.
// Strategy: walk up to the nearest declaration boundary; if the apply falls
// inside that declaration's explicit_binder/implicit_binder, or before the
// body field, it's a type annotation. We check byte ranges: a call is valid
// only if it overlaps the body range of the enclosing declaration.
static bool lean_is_in_type_position(TSNode node) {
    TSNode cur = ts_node_parent(node);
    for (int depth = 0; depth < LEAN_MAX_PARENT_DEPTH; depth++) {
        if (ts_node_is_null(cur)) {
            return false;
        }
        const char *pk = ts_node_type(cur);
        // Inside a binder — definitely type position
        if (strcmp(pk, "explicit_binder") == 0 || strcmp(pk, "implicit_binder") == 0 ||
            strcmp(pk, "instance_binder") == 0) {
            return true;
        }
        // At a declaration boundary: check if apply is inside the body field
        if (strcmp(pk, "def") == 0 || strcmp(pk, "theorem") == 0 || strcmp(pk, "instance") == 0 ||
            strcmp(pk, "abbrev") == 0 || strcmp(pk, "structure") == 0 ||
            strcmp(pk, "inductive") == 0) {
            // Check if apply comes after the type annotation.
            // Strategy: if the node starts after the end of the "type" field, it's in value
            // position. If there's no "type" field, allow the call (no annotation to filter).
            TSNode type_field = ts_node_child_by_field_name(cur, TS_FIELD("type"));
            if (ts_node_is_null(type_field)) {
                return false; // no type annotation → allow call
            }
            uint32_t type_end = ts_node_end_byte(type_field);
            uint32_t node_start = ts_node_start_byte(node);
            // If apply starts after the type annotation ends, it's a value (call)
            if (node_start > type_end) {
                return false;
            }
            return true; // apply is within or before type annotation → type position
        }
        cur = ts_node_parent(cur);
    }
    return false;
}

/* Resolve a selector_expression that may chain through call_expressions.
 * Go pattern: pb.NewFooClient(conn).GetBar → "pb.NewFooClient.GetBar"
 * Without this, cbm_node_text returns full text including args/parens.
 * Iteratively walks the chain: selector → operand(call) → function(selector) → ... */
static char *resolve_chained_selector(CBMArena *a, TSNode sel, const char *source) {
    TSNode operand = ts_node_child_by_field_name(sel, TS_FIELD("operand"));
    TSNode field = ts_node_child_by_field_name(sel, TS_FIELD("field"));
    if (ts_node_is_null(operand) || ts_node_is_null(field) ||
        strcmp(ts_node_type(operand), "call_expression") != 0) {
        return cbm_node_text(a, sel, source);
    }

    /* Operand is a call_expression — extract its callee iteratively.
     * Walk: call_expression → function field → if selector_expression, repeat. */
    char *method = cbm_node_text(a, field, source);
    TSNode inner = operand;
    enum { MAX_CHAIN_DEPTH = 4 };
    for (int depth = 0; depth < MAX_CHAIN_DEPTH; depth++) {
        TSNode fn = ts_node_child_by_field_name(inner, TS_FIELD("function"));
        if (ts_node_is_null(fn)) {
            break;
        }
        const char *fnk = ts_node_type(fn);
        if (strcmp(fnk, "selector_expression") == 0) {
            /* Check if this selector also chains through a call */
            TSNode inner_op = ts_node_child_by_field_name(fn, TS_FIELD("operand"));
            if (!ts_node_is_null(inner_op) &&
                strcmp(ts_node_type(inner_op), "call_expression") == 0) {
                inner = inner_op;
                continue;
            }
        }
        /* Reached a non-chained callee — extract its text */
        char *base = cbm_node_text(a, fn, source);
        if (base && method) {
            return cbm_arena_sprintf(a, "%s.%s", base, method);
        }
        return method;
    }

    /* Fallback: just return the method name */
    return method;
}

// Strip a trailing generic argument list ("<...>" / "[...]") from a type name,
// returning the bare type identifier. Mutates an arena-owned copy in place.
static char *strip_generic_args(char *t) {
    if (!t) {
        return NULL;
    }
    char *angle = strchr(t, '<');
    if (angle) {
        *angle = '\0';
    }
    char *brack = strchr(t, '[');
    if (brack) {
        *brack = '\0';
    }
    return t;
}

// Pull the constructed type name out of a constructor/instantiation node:
//   new_expression               (TS/JS)  -> `constructor`/`type` field or first type child
//   object_creation_expression   (Java/C#/PHP) -> `type` field or first type child
//   instance_expression          (Scala)  -> nested type in the wrapped type/call
// Returns the bare type name (generic args stripped) or NULL if not a
// constructor node / no type found. Constructor calls resolve to the class's
// constructor (or the class node) downstream, producing a CALLS edge.
static char *extract_constructor_callee(CBMArena *a, TSNode node, const char *source,
                                        const char *nk) {
    if (strcmp(nk, "new_expression") != 0 && strcmp(nk, "object_creation_expression") != 0 &&
        strcmp(nk, "instance_expression") != 0) {
        return NULL;
    }

    // Preferred: explicit fields used by the various grammars.
    static const char *type_fields[] = {"constructor", "type", "name", NULL};
    for (const char **f = type_fields; *f; f++) {
        TSNode tn = ts_node_child_by_field_name(node, *f, (uint32_t)strlen(*f));
        if (!ts_node_is_null(tn)) {
            const char *tk = ts_node_type(tn);
            // For a generic_type wrapper, descend to the bare name child.
            if (strcmp(tk, "generic_type") == 0 && ts_node_named_child_count(tn) > 0) {
                tn = ts_node_named_child(tn, 0);
            }
            char *t = strip_generic_args(cbm_node_text(a, tn, source));
            if (t && t[0]) {
                return t;
            }
        }
    }

    // Fallback: first type-like named child (covers grammars that don't expose
    // a field, e.g. Scala's instance_expression wraps the type directly).
    uint32_t nc = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode child = ts_node_named_child(node, i);
        const char *ck = ts_node_type(child);
        if (strcmp(ck, "type_identifier") == 0 || strcmp(ck, "identifier") == 0 ||
            strcmp(ck, "qualified_name") == 0 || strcmp(ck, "scoped_type_identifier") == 0 ||
            strcmp(ck, "qualified_identifier") == 0 || strcmp(ck, "name") == 0 ||
            strcmp(ck, "type") == 0 || strcmp(ck, "generic_type") == 0 ||
            strcmp(ck, "simple_type") == 0 || strcmp(ck, "stable_type_identifier") == 0 ||
            strcmp(ck, "user_type") == 0) {
            // Descend through a generic_type wrapper to the bare name.
            if (strcmp(ck, "generic_type") == 0 && ts_node_named_child_count(child) > 0) {
                child = ts_node_named_child(child, 0);
            }
            char *t = strip_generic_args(cbm_node_text(a, child, source));
            if (t && t[0]) {
                return t;
            }
        }
    }
    return NULL;
}

// Try common field-based callee resolution (function, name, method fields).
static char *extract_callee_from_fields(CBMArena *a, TSNode node, const char *source) {
    // Try "function" field
    TSNode func_node = ts_node_child_by_field_name(node, TS_FIELD("function"));
    if (!ts_node_is_null(func_node)) {
        const char *fk = ts_node_type(func_node);
        if (strcmp(fk, "selector_expression") == 0) {
            return resolve_chained_selector(a, func_node, source);
        }
        if (strcmp(fk, "identifier") == 0 || strcmp(fk, "simple_identifier") == 0 ||
            strcmp(fk, "attribute") == 0 || strcmp(fk, "member_expression") == 0 ||
            strcmp(fk, "field_expression") == 0 || strcmp(fk, "dot") == 0 ||
            strcmp(fk, "function") == 0 || strcmp(fk, "dotted_identifier") == 0 ||
            strcmp(fk, "member_access_expression") == 0 || strcmp(fk, "scoped_identifier") == 0 ||
            strcmp(fk, "qualified_identifier") == 0 ||
            /* ReScript: call_expression `function` field is a value_identifier
             * (or value_identifier_path for module-qualified calls). */
            strcmp(fk, "value_identifier") == 0 || strcmp(fk, "value_identifier_path") == 0) {
            return cbm_node_text(a, func_node, source);
        }
        // C++ explicit template call f<T>(args): the `function` field is a
        // template_function whose `name` child is the bare callee (identifier
        // "identity" or qualified_identifier "ns::f"). Without this the whole
        // "identity<int>" text would never be produced as a textual callee, so
        // no CALLS edge — and the LSP's lsp_template resolution has nothing to
        // attach to. Return the name child so the join recovers the bare method.
        if (strcmp(fk, "template_function") == 0) {
            TSNode tname = ts_node_child_by_field_name(func_node, TS_FIELD("name"));
            if (!ts_node_is_null(tname)) {
                return cbm_node_text(a, tname, source);
            }
        }
        // R member call: module$fn() — function node is an extract_operator
        // with lhs (object) and rhs (method). Emit "module.fn" so it resolves
        // like other member calls (#219). Previously dropped → no CALLS edge.
        if (strcmp(fk, "extract_operator") == 0) {
            TSNode lhs = ts_node_child_by_field_name(func_node, TS_FIELD("lhs"));
            TSNode rhs = ts_node_child_by_field_name(func_node, TS_FIELD("rhs"));
            if (!ts_node_is_null(rhs)) {
                char *rt = cbm_node_text(a, rhs, source);
                if (!ts_node_is_null(lhs)) {
                    char *lt = cbm_node_text(a, lhs, source);
                    if (lt && lt[0] && rt && rt[0]) {
                        return cbm_arena_sprintf(a, "%s.%s", lt, rt);
                    }
                }
                return rt;
            }
        }
    }

    // Try "name" field (Java method_invocation)
    TSNode name_node = ts_node_child_by_field_name(node, TS_FIELD("name"));
    if (!ts_node_is_null(name_node)) {
        char *name = cbm_node_text(a, name_node, source);
        TSNode obj = ts_node_child_by_field_name(node, TS_FIELD("object"));
        if (!ts_node_is_null(obj) && name) {
            char *obj_text = cbm_node_text(a, obj, source);
            if (obj_text && obj_text[0]) {
                return cbm_arena_sprintf(a, "%s.%s", obj_text, name);
            }
        }
        return name;
    }

    // Ruby: "method" + "receiver" fields
    TSNode method_node = ts_node_child_by_field_name(node, TS_FIELD("method"));
    if (!ts_node_is_null(method_node)) {
        char *method = cbm_node_text(a, method_node, source);
        TSNode recv = ts_node_child_by_field_name(node, TS_FIELD("receiver"));
        if (!ts_node_is_null(recv) && method) {
            char *recv_text = cbm_node_text(a, recv, source);
            if (recv_text && recv_text[0]) {
                return cbm_arena_sprintf(a, "%s.%s", recv_text, method);
            }
        }
        return method;
    }

    return NULL;
}

// Haskell/OCaml: extract callee from apply/infix nodes.
static char *extract_fp_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "apply") == 0 || strcmp(nk, "application_expression") == 0 ||
        strcmp(nk, "exp_apply") == 0) {
        if (ts_node_child_count(node) > 0) {
            TSNode callee = ts_node_child(node, 0);
            const char *ck = ts_node_type(callee);
            if (strcmp(ck, "identifier") == 0 || strcmp(ck, "variable") == 0 ||
                strcmp(ck, "constructor") == 0 || strcmp(ck, "value_path") == 0 ||
                /* PureScript: exp_apply's function head is an `exp_name` whose
                 * text is the (possibly qualified) function name. */
                strcmp(ck, "exp_name") == 0) {
                return cbm_node_text(a, callee, source);
            }
            /* Curried application `f a b` nests exp_apply/apply — descend the
             * function head to recover the leftmost callee. */
            if (strcmp(ck, "exp_apply") == 0 || strcmp(ck, "apply") == 0 ||
                strcmp(ck, "application_expression") == 0) {
                return extract_fp_callee(a, callee, source, ck);
            }
        }
    }
    if (strcmp(nk, "infix") == 0 || strcmp(nk, "infix_expression") == 0) {
        TSNode op = ts_node_child_by_field_name(node, TS_FIELD("operator"));
        if (!ts_node_is_null(op)) {
            return cbm_node_text(a, op, source);
        }
        enum { INFIX_MIN_CHILDREN = 3, INFIX_OP_IDX = 1 };
        if (ts_node_child_count(node) >= INFIX_MIN_CHILDREN) {
            return cbm_node_text(a, ts_node_child(node, INFIX_OP_IDX), source);
        }
    }
    return NULL;
}

// Wolfram: extract callee from apply, skipping LHS of set definitions.
static char *extract_wolfram_callee(CBMArena *a, TSNode node, const char *source) {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent)) {
        const char *pk = ts_node_type(parent);
        if ((strcmp(pk, "set_delayed_top") == 0 || strcmp(pk, "set_top") == 0 ||
             strcmp(pk, "set_delayed") == 0 || strcmp(pk, "set") == 0) &&
            ts_node_named_child_count(parent) > 0 &&
            ts_node_eq(ts_node_named_child(parent, 0), node)) {
            return NULL;
        }
    }
    if (ts_node_named_child_count(node) > 0) {
        TSNode head = ts_node_named_child(node, 0);
        const char *hk = ts_node_type(head);
        if (strcmp(hk, "user_symbol") == 0 || strcmp(hk, "builtin_symbol") == 0) {
            return cbm_node_text(a, head, source);
        }
    }
    return NULL;
}

// Language-specific callee extraction for FP and niche languages.
// Swift callee extraction from call/constructor expressions.
static char *extract_swift_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "call_expression") != 0 && strcmp(nk, "constructor_expression") != 0) {
        return NULL;
    }
    if (ts_node_named_child_count(node) > 0) {
        TSNode callee = ts_node_named_child(node, 0);
        const char *ck = ts_node_type(callee);
        if (strcmp(ck, "simple_identifier") == 0 || strcmp(ck, "navigation_expression") == 0) {
            return cbm_node_text(a, callee, source);
        }
    }
    return NULL;
}

// A Perl sub/method name is an identifier: it starts with a letter or '_',
// contains only [A-Za-z0-9_] plus the '::' package separator, and is never a
// string/config literal. tree-sitter-perl mis-parses config lines in .cgi /
// heredoc-heavy files into call-shaped nodes whose "callee" is a dotted config
// token (e.g. "log4perl.appender.File.utf8"); rejecting non-identifier text
// here stops those from becoming bogus CALLS edges. Any '.', whitespace, quote,
// or '/' disqualifies the token.
static bool perl_is_identifier_callee(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    unsigned char c0 = (unsigned char)name[0];
    if (!(isalpha(c0) || c0 == '_')) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_') {
            continue;
        }
        if (c == ':') {
            // Only the '::' package separator is allowed: require an adjacent
            // pair, and reject a lone ':', ':::', or a trailing '::'.
            if (p[1] != ':' || p[2] == ':' || p[2] == '\0') {
                return false;
            }
            p++; // consume the second ':'; the loop's p++ moves past the pair
            continue;
        }
        return false; // '.', space, quote, '/', etc. → not a sub/method name
    }
    return true;
}

// Callee extraction for scripting languages (Elixir, Perl, PHP, Kotlin, MATLAB).
static char *extract_scripting_callee(CBMArena *a, TSNode node, const char *source,
                                      CBMLanguage lang, const char *nk) {
    if (lang == CBM_LANG_ELIXIR && strcmp(nk, "call") == 0 && ts_node_child_count(node) > 0) {
        TSNode first = ts_node_child(node, 0);
        const char *fk = ts_node_type(first);
        if (strcmp(fk, "identifier") == 0 || strcmp(fk, "dot") == 0) {
            return cbm_node_text(a, first, source);
        }
        return NULL;
    }
    if (lang == CBM_LANG_PERL && ts_node_child_count(node) > 0) {
        // Pull the actual sub/method name token rather than blindly taking
        // child(0). Grammar (verified against the vendored parser):
        //   method_call_expression   : field `method`   ($obj->m / Class->m)
        //   function_call_expression : field `function` (foo(); name with '.'
        //                              from a config-string misparse lands here)
        //   ambiguous_function_call_expression : field `function`
        //   func1op_call_expression  : builtin keyword as child(0) (no field)
        TSNode name_node = ts_node_child_by_field_name(node, TS_FIELD("method"));
        if (ts_node_is_null(name_node)) {
            name_node = ts_node_child_by_field_name(node, TS_FIELD("function"));
        }
        if (ts_node_is_null(name_node)) {
            name_node = ts_node_child(node, 0);
        }
        char *pn = cbm_node_text(a, name_node, source);
        // Reject anything that is not a bare Perl sub/method identifier (config
        // strings, quoted literals, paths) so no spurious CALLS edge is emitted.
        return perl_is_identifier_callee(pn) ? pn : NULL;
    }
    if (lang == CBM_LANG_PHP) {
        TSNode func_node = ts_node_child_by_field_name(node, TS_FIELD("function"));
        if (ts_node_is_null(func_node)) {
            func_node = ts_node_child_by_field_name(node, TS_FIELD("name"));
        }
        char *pn = ts_node_is_null(func_node) ? NULL : cbm_node_text(a, func_node, source);
        return pn;
    }
    if (lang == CBM_LANG_KOTLIN && ts_node_child_count(node) > 0) {
        return cbm_node_text(a, ts_node_child(node, 0), source);
    }
    if (lang == CBM_LANG_MATLAB && strcmp(nk, "command") == 0 && ts_node_child_count(node) > 0) {
        return cbm_node_text(a, ts_node_child(node, 0), source);
    }
    return NULL;
}

// ObjC: extract callee from message_expression selector.
static char *extract_objc_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "message_expression") != 0) {
        return NULL;
    }
    TSNode selector = ts_node_child_by_field_name(node, TS_FIELD("selector"));
    return ts_node_is_null(selector) ? NULL : cbm_node_text(a, selector, source);
}

// Erlang: extract callee from call node's first child.
static char *extract_erlang_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "call") != 0 || ts_node_child_count(node) == 0) {
        return NULL;
    }
    return cbm_node_text(a, ts_node_child(node, 0), source);
}

// Lisp dialects: a call is a list (`list` / `list_lit`) whose head (first named
// child) is the function symbol (`symbol` / `sym_lit`). Generic field/first-child
// extraction misses it because the head is not an `identifier` node.
static char *extract_lisp_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "list") != 0 && strcmp(nk, "list_lit") != 0) {
        return NULL;
    }
    if (ts_node_named_child_count(node) > 0) {
        TSNode head = ts_node_named_child(node, 0);
        const char *hk = ts_node_type(head);
        if (strcmp(hk, "symbol") == 0 || strcmp(hk, "sym_lit") == 0 ||
            strcmp(hk, "identifier") == 0) {
            return cbm_node_text(a, head, source);
        }
    }
    return NULL;
}

// F#: application_expression head is a long_identifier_or_op wrapper, not a bare
// identifier, so extract_fp_callee's accepted-type list would miss it.
static char *extract_fsharp_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "application_expression") != 0 || ts_node_named_child_count(node) == 0) {
        return NULL;
    }
    TSNode head = ts_node_named_child(node, 0);
    const char *hk = ts_node_type(head);
    if (strcmp(hk, "long_identifier_or_op") == 0 || strcmp(hk, "long_identifier") == 0 ||
        strcmp(hk, "identifier") == 0) {
        return cbm_node_text(a, head, source);
    }
    return NULL;
}

// CSS: a `call_expression` (e.g. `url(...)`, `calc(...)`) carries its callee on a
// plain `function_name` child rather than a `function`/`name` field, so generic
// field/first-child resolution misses it.
static char *extract_css_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "call_expression") != 0) {
        return NULL;
    }
    TSNode fn = cbm_find_child_by_kind(node, "function_name");
    return ts_node_is_null(fn) ? NULL : cbm_node_text(a, fn, source);
}

// PowerShell: a `command` node's callee is its `command_name` child.
static char *extract_powershell_callee(CBMArena *a, TSNode node, const char *source,
                                       const char *nk) {
    if (strcmp(nk, "command") != 0) {
        return NULL;
    }
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        TSNode c = ts_node_named_child(node, i);
        if (strcmp(ts_node_type(c), "command_name") == 0) {
            return cbm_node_text(a, c, source);
        }
    }
    return NULL;
}

// Ada: procedure_call_statement / function_call carry the callee in a `name` field.
static char *extract_ada_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "procedure_call_statement") != 0 && strcmp(nk, "function_call") != 0) {
        return NULL;
    }
    TSNode name = ts_node_child_by_field_name(node, TS_FIELD("name"));
    if (!ts_node_is_null(name)) {
        return cbm_node_text(a, name, source);
    }
    if (ts_node_named_child_count(node) > 0) {
        TSNode head = ts_node_named_child(node, 0);
        const char *hk = ts_node_type(head);
        if (strcmp(hk, "name") == 0 || strcmp(hk, "identifier") == 0) {
            return cbm_node_text(a, head, source);
        }
    }
    return NULL;
}

// Solidity: a call_expression's callee is on the `function` field, wrapped in an
// `expression` node (call_expression -> function:expression -> identifier). Descend
// left-most through expression wrappers until we reach the identifier/member.
static char *extract_solidity_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "call_expression") != 0 && strcmp(nk, "call") != 0) {
        return NULL;
    }
    TSNode head = ts_node_child_by_field_name(node, TS_FIELD("function"));
    if (ts_node_is_null(head) && ts_node_named_child_count(node) > 0) {
        head = ts_node_named_child(node, 0);
    }
    // Unwrap nested `expression` wrappers down to the callee identifier/member.
    for (int depth = 0; depth < 4 && !ts_node_is_null(head); depth++) {
        const char *hk = ts_node_type(head);
        if (strcmp(hk, "identifier") == 0 || strcmp(hk, "member_expression") == 0 ||
            strcmp(hk, "member_access") == 0) {
            return cbm_node_text(a, head, source);
        }
        if (strcmp(hk, "expression") == 0 && ts_node_named_child_count(head) > 0) {
            head = ts_node_named_child(head, 0);
            continue;
        }
        break;
    }
    return NULL;
}

// Groovy: function_call's first named child is the callee identifier (the generic
// first-child fallback misses it because child 0 is anonymous).
static char *extract_groovy_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "function_call") != 0 && strcmp(nk, "juxt_function_call") != 0) {
        return NULL;
    }
    if (ts_node_named_child_count(node) > 0) {
        TSNode head = ts_node_named_child(node, 0);
        if (!ts_node_is_null(head) && strcmp(ts_node_type(head), "identifier") == 0) {
            return cbm_node_text(a, head, source);
        }
    }
    return NULL;
}

// WGSL: callee is nested type_constructor_or_function_call_expression ->
// type_declaration -> identifier. Descend left-most until an identifier.
static char *extract_wgsl_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "type_constructor_or_function_call_expression") != 0) {
        return NULL;
    }
    TSNode head = node;
    while (ts_node_named_child_count(head) > 0 && strcmp(ts_node_type(head), "identifier") != 0) {
        head = ts_node_named_child(head, 0);
    }
    if (strcmp(ts_node_type(head), "identifier") == 0) {
        return cbm_node_text(a, head, source);
    }
    return NULL;
}

// Dart: the invocation `selector` (the `(...)` part) follows the callee
// identifier as a sibling; `new_expression`'s first named child is the type.
static char *extract_dart_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "selector") == 0) {
        TSNode prev = ts_node_prev_named_sibling(node);
        if (!ts_node_is_null(prev) && strcmp(ts_node_type(prev), "identifier") == 0) {
            return cbm_node_text(a, prev, source);
        }
        return NULL;
    }
    if (strcmp(nk, "new_expression") == 0 && ts_node_named_child_count(node) > 0) {
        TSNode head = ts_node_named_child(node, 0);
        const char *hk = ts_node_type(head);
        if (strcmp(hk, "identifier") == 0 || strcmp(hk, "type_identifier") == 0) {
            return cbm_node_text(a, head, source);
        }
    }
    return NULL;
}

// SCSS: an `@include foo;` is an include_statement whose callee is its
// `identifier` child (the mixin name).
static char *extract_scss_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "include_statement") == 0) {
        TSNode id = cbm_find_child_by_kind(node, "identifier");
        return ts_node_is_null(id) ? NULL : cbm_node_text(a, id, source);
    }
    /* SCSS @function call `double($x)` is a call_expression whose callee is a
     * `function_name` child (there is no `function` field), so the generic
     * field-based resolver returns NULL and the call is dropped — no CALLS edge
     * to the in-file @function. */
    if (strcmp(nk, "call_expression") == 0) {
        TSNode fn = cbm_find_child_by_kind(node, "function_name");
        if (!ts_node_is_null(fn)) {
            return cbm_node_text(a, fn, source);
        }
    }
    return NULL;
}

// SQL: an `invocation` node's callee is nested object_reference > `name` field
// (the same shape as a create_function's name).
static char *extract_sql_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "invocation") != 0) {
        return NULL;
    }
    TSNode oref = cbm_find_child_by_kind(node, "object_reference");
    if (ts_node_is_null(oref)) {
        return NULL;
    }
    TSNode nm = ts_node_child_by_field_name(oref, TS_FIELD("name"));
    return ts_node_is_null(nm) ? NULL : cbm_node_text(a, nm, source);
}

// COBOL: a `CALL 'HELPER'` is a call_statement whose `x` field is a string
// literal naming the called program; the callee is that string sans quotes.
static char *extract_cobol_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "call_statement") != 0) {
        return NULL;
    }
    TSNode x = ts_node_child_by_field_name(node, TS_FIELD("x"));
    if (ts_node_is_null(x)) {
        x = cbm_find_child_by_kind(node, "string");
    }
    if (ts_node_is_null(x)) {
        return NULL;
    }
    char *text = cbm_node_text(a, x, source);
    return (char *)strip_and_validate_string_arg(a, text);
}

// Elm: a `function_call_expr` has a `target` field; the callee identifier is
// target > value_expr > `name` field (value_qid) > lower_case_identifier.
static char *extract_elm_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "function_call_expr") != 0) {
        return NULL;
    }
    TSNode target = ts_node_child_by_field_name(node, TS_FIELD("target"));
    if (ts_node_is_null(target)) {
        return NULL;
    }
    TSNode ve = strcmp(ts_node_type(target), "value_expr") == 0
                    ? target
                    : cbm_find_child_by_kind(target, "value_expr");
    if (ts_node_is_null(ve)) {
        return NULL;
    }
    TSNode qid = ts_node_child_by_field_name(ve, TS_FIELD("name"));
    if (ts_node_is_null(qid)) {
        qid = cbm_find_child_by_kind(ve, "value_qid");
    }
    if (ts_node_is_null(qid)) {
        return NULL;
    }
    TSNode id = cbm_find_child_by_kind(qid, "lower_case_identifier");
    if (ts_node_is_null(id)) {
        // module-qualified call: emit the whole qualified id text
        return cbm_node_text(a, qid, source);
    }
    return cbm_node_text(a, id, source);
}

// Jsonnet: a `functioncall` node's callee is its first `id` child (the called
// binding name); the generic field path misses it (no `function`/`name` field).
static char *extract_jsonnet_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "functioncall") != 0) {
        return NULL;
    }
    TSNode id = cbm_find_child_by_kind(node, "id");
    return ts_node_is_null(id) ? NULL : cbm_node_text(a, id, source);
}

// Nickel: function application is `applicative` and curries left-associatively:
// `f x y` parses as `(applicative t1:(applicative t1:f t2:x) t2:y)`. A real call
// node carries a `t2` (argument) field; a bare value (`applicative
// (record_operand (atom (ident))))` wraps every expression and has no `t2`, so it
// is NOT a call. We also skip applicatives whose parent is itself an applicative
// (the inner partial-application nodes) so a curried call emits exactly one edge,
// keyed on the leftmost ident reached by descending the `t1` chain.
// (`infix_expr` is binary operator application, not a call, and is excluded from
// nickel_call_types.)
static char *extract_nickel_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "applicative") != 0) {
        return NULL;
    }
    // Not an application unless it has an argument (`t2`).
    if (ts_node_is_null(ts_node_child_by_field_name(node, TS_FIELD("t2")))) {
        return NULL;
    }
    // Emit only at the outermost applicative of a curried chain.
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && strcmp(ts_node_type(parent), "applicative") == 0) {
        return NULL;
    }
    enum { NICKEL_APPLY_DEPTH = 8 };
    TSNode cur = node;
    for (int depth = 0; depth < NICKEL_APPLY_DEPTH && !ts_node_is_null(cur); depth++) {
        const char *ck = ts_node_type(cur);
        if (strcmp(ck, "ident") == 0) {
            return cbm_node_text(a, cur, source);
        }
        // Descend the function side: the `t1` field for curried applicatives, or
        // the wrapper's first named child (record_operand -> atom -> ident).
        TSNode next = ts_node_child_by_field_name(cur, TS_FIELD("t1"));
        if (ts_node_is_null(next) && ts_node_named_child_count(cur) > 0) {
            next = ts_node_named_child(cur, 0);
        }
        if (ts_node_is_null(next) || ts_node_eq(next, cur)) {
            break;
        }
        cur = next;
    }
    return NULL;
}

// Typst: a `call` node's callee is its `item` field (an ident), matching the
// def-side resolution of `#let greet(name) = ...`.
static char *extract_typst_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "call") != 0) {
        return NULL;
    }
    TSNode item = ts_node_child_by_field_name(node, TS_FIELD("item"));
    return ts_node_is_null(item) ? NULL : cbm_node_text(a, item, source);
}

// Meson: a builtin invocation (`executable(...)`, `dependency(...)`) is a
// `normal_command` whose `command` field is the called identifier.
static char *extract_meson_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "normal_command") != 0) {
        return NULL;
    }
    TSNode cmd = ts_node_child_by_field_name(node, TS_FIELD("command"));
    return ts_node_is_null(cmd) ? NULL : cbm_node_text(a, cmd, source);
}

// Descend left-most through wrapper nodes to the first identifier-bearing leaf.
// Used by HDL call nodes whose callee identifier is nested under one or more
// grammar wrappers (Verilog tf_call -> simple_identifier; SystemVerilog
// tf_call -> hierarchical_identifier -> simple_identifier).
static char *first_leaf_identifier(CBMArena *a, TSNode node, const char *source) {
    TSNode cur = node;
    for (int depth = 0; depth < 8 && !ts_node_is_null(cur); depth++) {
        const char *k = ts_node_type(cur);
        if (strcmp(k, "simple_identifier") == 0 || strcmp(k, "identifier") == 0 ||
            strcmp(k, "word") == 0 || strcmp(k, "name") == 0 || strcmp(k, "qid") == 0) {
            char *t = cbm_node_text(a, cur, source);
            return (t && t[0]) ? t : NULL;
        }
        if (ts_node_named_child_count(cur) == 0) {
            return NULL;
        }
        cur = ts_node_named_child(cur, 0);
    }
    return NULL;
}

// Verilog / SystemVerilog: a function_subroutine_call wraps
// subroutine_call -> tf_call -> [hierarchical_identifier ->] simple_identifier.
// Descend to the first identifier leaf to name the callee.
static char *extract_hdl_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "function_subroutine_call") != 0 && strcmp(nk, "subroutine_call") != 0 &&
        strcmp(nk, "tf_call") != 0 && strcmp(nk, "system_tf_call") != 0) {
        return NULL;
    }
    return first_leaf_identifier(a, node, source);
}

// VHDL: `add(x, 1)` parses as `(name (library_function) (parenthesis_group ...))`
// inside a `simple_expression` (the function-call / indexed-name ambiguity). The
// call_node_types set targets `parenthesis_group`; the callee is its immediately
// preceding named sibling (a `library_function`/`identifier`/`name` token).
static char *extract_vhdl_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "parenthesis_group") != 0) {
        return NULL;
    }
    TSNode prev = ts_node_prev_named_sibling(node);
    if (ts_node_is_null(prev)) {
        return NULL;
    }
    const char *pk = ts_node_type(prev);
    if (strcmp(pk, "library_function") == 0 || strcmp(pk, "identifier") == 0 ||
        strcmp(pk, "name") == 0 || strcmp(pk, "simple_name") == 0) {
        char *t = cbm_node_text(a, prev, source);
        return (t && t[0]) ? t : NULL;
    }
    return NULL;
}

// NASM: a `call`/`jmp`-style instruction is an `actual_instruction` whose
// `instruction:` field is the mnemonic word and whose first operand word is the
// target label. Only treat call/jump mnemonics as calls; everything else (add,
// mov, ret, ...) is plain data-flow, not a call.
static char *extract_nasm_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "actual_instruction") != 0) {
        return NULL;
    }
    TSNode mnem = ts_node_child_by_field_name(node, TS_FIELD("instruction"));
    if (ts_node_is_null(mnem)) {
        return NULL;
    }
    char *m = cbm_node_text(a, mnem, source);
    if (!m || (strcmp(m, "call") != 0 && strcmp(m, "jmp") != 0 && strcmp(m, "je") != 0 &&
               strcmp(m, "jne") != 0 && strcmp(m, "jz") != 0 && strcmp(m, "jnz") != 0)) {
        return NULL;
    }
    TSNode ops = ts_node_child_by_field_name(node, TS_FIELD("operands"));
    if (ts_node_is_null(ops) || ts_node_named_child_count(ops) == 0) {
        return NULL;
    }
    return first_leaf_identifier(a, ts_node_named_child(ops, 0), source);
}

// LLVM-IR: a `call`/`invoke` is an `instruction_call` whose `callee:` field is a
// `value -> var -> global_var` chain (e.g. `@inner`). Strip the leading sigil.
static char *extract_llvm_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "instruction_call") != 0) {
        return NULL;
    }
    TSNode callee = ts_node_child_by_field_name(node, TS_FIELD("callee"));
    if (ts_node_is_null(callee)) {
        return NULL;
    }
    char *t = first_leaf_identifier(a, callee, source);
    if (!t) {
        t = cbm_node_text(a, callee, source);
    }
    if (t && (t[0] == '@' || t[0] == '%')) {
        return t + 1;
    }
    return t;
}

// FunC: a `function_application` carries the callee on its `function:` field.
static char *extract_func_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "function_application") != 0) {
        return NULL;
    }
    TSNode fn = ts_node_child_by_field_name(node, TS_FIELD("function"));
    return ts_node_is_null(fn) ? NULL : cbm_node_text(a, fn, source);
}

// Nix: an `apply_expression` (`f x`) carries the applied function on its
// `function:` field. The head is a `variable_expression` whose `name` is the
// callee identifier; curried application (`f x y`) nests apply_expressions, so
// descend the `function` chain to the head variable_expression. The generic
// field resolver does not recognise `variable_expression`, so without this the
// call to `addOne` would never be captured.
static char *extract_nix_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "apply_expression") != 0) {
        return NULL;
    }
    TSNode fn = ts_node_child_by_field_name(node, TS_FIELD("function"));
    for (int depth = 0; depth < 8 && !ts_node_is_null(fn); depth++) {
        const char *fk = ts_node_type(fn);
        if (strcmp(fk, "apply_expression") == 0) {
            fn = ts_node_child_by_field_name(fn, TS_FIELD("function"));
            continue;
        }
        if (strcmp(fk, "variable_expression") == 0) {
            TSNode nm = ts_node_child_by_field_name(fn, TS_FIELD("name"));
            return ts_node_is_null(nm) ? NULL : cbm_node_text(a, nm, source);
        }
        if (strcmp(fk, "identifier") == 0) {
            return cbm_node_text(a, fn, source);
        }
        return NULL;
    }
    return NULL;
}

// Agda: function application `f x y` parses as an `expr` whose named children are
// `atom`s (no dedicated application node). Treat an `expr` with >= 2 atom children
// as a call whose callee is the head atom's identifier.
static char *extract_agda_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "expr") != 0 || ts_node_named_child_count(node) < 2) {
        return NULL;
    }
    TSNode head = ts_node_named_child(node, 0);
    if (strcmp(ts_node_type(head), "atom") != 0) {
        return NULL;
    }
    return first_leaf_identifier(a, head, source);
}

// Make: `$(shell ...)` is a `shell_function` node; the callee is the literal
// `shell` keyword. tree-sitter-make also exposes `function_call` for other
// builtins ($(wildcard ...), $(patsubst ...)).
static char *extract_make_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "shell_function") == 0) {
        return cbm_arena_strndup(a, "shell", 5);
    }
    if (strcmp(nk, "function_call") == 0) {
        TSNode fn = ts_node_child_by_field_name(node, TS_FIELD("function"));
        if (ts_node_is_null(fn) && ts_node_named_child_count(node) > 0) {
            fn = ts_node_named_child(node, 0);
        }
        return ts_node_is_null(fn) ? NULL : cbm_node_text(a, fn, source);
    }
    return NULL;
}

// Just: a recipe dependency `recipe: dep` is a `dependency` node whose `name:`
// field is the referenced recipe.
static char *extract_just_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "dependency") != 0) {
        return NULL;
    }
    TSNode name = ts_node_child_by_field_name(node, TS_FIELD("name"));
    if (ts_node_is_null(name) && ts_node_named_child_count(node) > 0) {
        name = ts_node_named_child(node, 0);
    }
    return ts_node_is_null(name) ? NULL : cbm_node_text(a, name, source);
}

// Puppet: `include foo` is an `include_statement`; the callee is the literal
// `include` keyword (the class/identifier args are resolved as separate refs).
static char *extract_puppet_callee(CBMArena *a, TSNode node, const char *source, const char *nk) {
    if (strcmp(nk, "include_statement") == 0) {
        return cbm_arena_strndup(a, "include", 7);
    }
    if (strcmp(nk, "function_call") == 0) {
        if (ts_node_named_child_count(node) > 0) {
            TSNode head = ts_node_named_child(node, 0);
            if (strcmp(ts_node_type(head), "identifier") == 0) {
                return cbm_node_text(a, head, source);
            }
        }
    }
    return NULL;
}

static char *extract_callee_lang_specific(CBMArena *a, TSNode node, const char *source,
                                          CBMLanguage lang) {
    const char *nk = ts_node_type(node);

    /* Python dict-dispatch call `funcs["a"](v)`: the call's `function` field is a
     * subscript whose base is the identifier holding the dispatch table. Emit the
     * base identifier ("funcs") as the textual callee so a CALLS edge exists; the
     * py-LSP resolves it to the real target and joins via `reason` (lsp_resolve.h,
     * lsp_dict_dispatch). Gated to the literal-string-key shape the LSP handles so
     * other subscript calls (arr[i]()) are unaffected. */
    if (lang == CBM_LANG_PYTHON && strcmp(nk, "call") == 0) {
        TSNode fnf = ts_node_child_by_field_name(node, TS_FIELD("function"));
        if (!ts_node_is_null(fnf) && strcmp(ts_node_type(fnf), "subscript") == 0) {
            TSNode val = ts_node_child_by_field_name(fnf, TS_FIELD("value"));
            TSNode idx = ts_node_child_by_field_name(fnf, TS_FIELD("subscript"));
            if (!ts_node_is_null(val) && !ts_node_is_null(idx) &&
                strcmp(ts_node_type(val), "identifier") == 0 &&
                strcmp(ts_node_type(idx), "string") == 0) {
                return cbm_node_text(a, val, source);
            }
        }
    }

    if (lang == CBM_LANG_JSONNET) {
        char *c = extract_jsonnet_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_NICKEL) {
        char *c = extract_nickel_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_TYPST) {
        char *c = extract_typst_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_MESON) {
        char *c = extract_meson_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }

    if (lang == CBM_LANG_SCSS) {
        char *c = extract_scss_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_CSS) {
        char *c = extract_css_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_SQL) {
        char *c = extract_sql_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_COBOL) {
        char *c = extract_cobol_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }
    if (lang == CBM_LANG_ELM) {
        char *c = extract_elm_callee(a, node, source, nk);
        return c ? c : extract_scripting_callee(a, node, source, lang, nk);
    }

    if (lang == CBM_LANG_CLOJURE || lang == CBM_LANG_COMMONLISP || lang == CBM_LANG_SCHEME ||
        lang == CBM_LANG_FENNEL || lang == CBM_LANG_RACKET || lang == CBM_LANG_EMACSLISP) {
        return extract_lisp_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_FSHARP) {
        return extract_fsharp_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_POWERSHELL) {
        return extract_powershell_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_ADA) {
        return extract_ada_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_SOLIDITY) {
        return extract_solidity_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_GROOVY) {
        return extract_groovy_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_WGSL) {
        return extract_wgsl_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_DART) {
        return extract_dart_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_OBJC) {
        return extract_objc_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_ERLANG) {
        return extract_erlang_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_HASKELL || lang == CBM_LANG_OCAML || lang == CBM_LANG_PURESCRIPT) {
        return extract_fp_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_WOLFRAM && strcmp(nk, "apply") == 0) {
        return extract_wolfram_callee(a, node, source);
    }
    if (lang == CBM_LANG_SWIFT) {
        return extract_swift_callee(a, node, source, nk);
    }
    if (lang == CBM_LANG_VERILOG || lang == CBM_LANG_SYSTEMVERILOG) {
        char *c = extract_hdl_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_VHDL) {
        char *c = extract_vhdl_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_NASM) {
        char *c = extract_nasm_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_LLVM_IR) {
        char *c = extract_llvm_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_FUNC) {
        char *c = extract_func_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_AGDA) {
        char *c = extract_agda_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_NIX) {
        char *c = extract_nix_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_MAKEFILE) {
        char *c = extract_make_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_JUST) {
        char *c = extract_just_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_PUPPET) {
        char *c = extract_puppet_callee(a, node, source, nk);
        if (c) {
            return c;
        }
    }
    if (lang == CBM_LANG_OBJECTSCRIPT_UDL || lang == CBM_LANG_OBJECTSCRIPT_ROUTINE) {
        // ##class(Pkg.Class).Method() -> "Pkg.Class.Method"
        if (strcmp(nk, "class_method_call") == 0) {
            TSNode class_ref = cbm_find_child_by_kind(node, "class_ref");
            TSNode method_name = cbm_find_child_by_kind(node, "method_name");
            if (!ts_node_is_null(class_ref) && !ts_node_is_null(method_name)) {
                TSNode cname = cbm_find_child_by_kind(class_ref, "class_name");
                if (ts_node_is_null(cname)) {
                    return NULL;
                }
                char *cls = cbm_node_text(a, cname, source);
                if (!cls || !cls[0]) {
                    return NULL;
                }
                TSNode mname_ident = ts_node_named_child_count(method_name) > 0
                                         ? ts_node_named_child(method_name, 0)
                                         : (TSNode){0};
                if (ts_node_is_null(mname_ident)) {
                    return cls;
                }
                char *meth = cbm_node_text(a, mname_ident, source);
                if (!meth || !meth[0]) {
                    return cls;
                }
                return cbm_arena_sprintf(a, "%s.%s", cls, meth);
            }
            return NULL;
        }
        // $$label^routine extrinsic / routine tag call -> the line_ref text
        if (strcmp(nk, "routine_tag_call") == 0) {
            TSNode line_ref = cbm_find_child_by_kind(node, "line_ref");
            if (!ts_node_is_null(line_ref)) {
                return cbm_node_text(a, line_ref, source);
            }
            return NULL;
        }
        // $$$Macro(...) -> raw "$$$Name" callee (expanded later in handle_calls)
        if (strcmp(nk, "macro") == 0) {
            char *raw = cbm_node_text(a, node, source);
            if (!raw || raw[0] != '$' || raw[1] != '$' || raw[2] != '$') {
                return NULL;
            }
            char *name_start = raw + 3;
            char *paren = strchr(name_start, '(');
            if (paren) {
                *paren = '\0';
            }
            if (!name_start[0]) {
                return NULL;
            }
            return cbm_arena_sprintf(a, "$$$%s", name_start);
        }
        return NULL;
    }

    return extract_scripting_callee(a, node, source, lang, nk);
}

// Extract callee name from a call node
/* #952: compose Laravel group prefixes into a route path. Walks UP from a
 * route-registration call: every enclosing anonymous function that is the
 * argument of a `->group(...)` member call contributes the `prefix('...')`
 * (or `Route::prefix('...')`) found on that group call's receiver chain.
 * Chain methods that don't shape the path (middleware, name, as, domain)
 * are skipped. Outer groups accumulate before inner ones; nested groups
 * compose left-to-right. Returns the composed path (arena) or NULL when no
 * enclosing group carries a prefix. */
enum { PHP_GROUP_WALK_MAX = 64, PHP_PREFIX_PARTS_MAX = 8 };

static const char *php_chain_prefix_arg(CBMArena *a, TSNode call_node, const char *source) {
    /* call_node is a member_call_expression / scoped_call_expression whose
     * name is `prefix`; return its first string argument (unquoted). */
    TSNode args = ts_node_child_by_field_name(call_node, TS_FIELD("arguments"));
    if (ts_node_is_null(args)) {
        return NULL;
    }
    uint32_t nc = ts_node_named_child_count(args);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode arg = ts_node_named_child(args, i);
        TSNode inner = arg;
        if (strcmp(ts_node_type(arg), "argument") == 0 && ts_node_named_child_count(arg) > 0) {
            inner = ts_node_named_child(arg, 0);
        }
        if (strcmp(ts_node_type(inner), "string") == 0 ||
            strcmp(ts_node_type(inner), "encapsed_string") == 0) {
            char *txt = cbm_node_text(a, inner, source);
            if (txt && txt[0]) {
                size_t len = strlen(txt);
                if (len >= 2 && (txt[0] == '\'' || txt[0] == '"')) {
                    txt[len - 1] = '\0';
                    txt++;
                }
                return txt[0] ? txt : NULL;
            }
        }
    }
    return NULL;
}

static const char *php_group_prefix_for_call(CBMArena *a, TSNode node, const char *source) {
    const char *parts[PHP_PREFIX_PARTS_MAX];
    int part_count = 0;
    TSNode cur = ts_node_parent(node);
    for (int depth = 0; depth < PHP_GROUP_WALK_MAX && !ts_node_is_null(cur); depth++) {
        if (strcmp(ts_node_type(cur), "anonymous_function") == 0 ||
            strcmp(ts_node_type(cur), "arrow_function") == 0) {
            /* Is this closure the argument of a ->group(...) call? Walk to
             * the enclosing call and check its method name. */
            TSNode p = ts_node_parent(cur);
            while (!ts_node_is_null(p) && strcmp(ts_node_type(p), "member_call_expression") != 0 &&
                   strcmp(ts_node_type(p), "scoped_call_expression") != 0) {
                if (strcmp(ts_node_type(p), "statement") == 0 ||
                    strcmp(ts_node_type(p), "expression_statement") == 0) {
                    break; /* left the argument position */
                }
                p = ts_node_parent(p);
            }
            if (!ts_node_is_null(p) && (strcmp(ts_node_type(p), "member_call_expression") == 0 ||
                                        strcmp(ts_node_type(p), "scoped_call_expression") == 0)) {
                TSNode gname = ts_node_child_by_field_name(p, TS_FIELD("name"));
                char *gtxt = ts_node_is_null(gname) ? NULL : cbm_node_text(a, gname, source);
                if (gtxt && strcmp(gtxt, "group") == 0) {
                    /* Scan the receiver chain for prefix('...'). */
                    TSNode recv = ts_node_child_by_field_name(p, TS_FIELD("object"));
                    for (int hops = 0; hops < PHP_GROUP_WALK_MAX && !ts_node_is_null(recv);
                         hops++) {
                        const char *rk = ts_node_type(recv);
                        if (strcmp(rk, "member_call_expression") == 0 ||
                            strcmp(rk, "scoped_call_expression") == 0) {
                            TSNode rname = ts_node_child_by_field_name(recv, TS_FIELD("name"));
                            char *rtxt =
                                ts_node_is_null(rname) ? NULL : cbm_node_text(a, rname, source);
                            if (rtxt && strcmp(rtxt, "prefix") == 0) {
                                const char *pf = php_chain_prefix_arg(a, recv, source);
                                if (pf && part_count < PHP_PREFIX_PARTS_MAX) {
                                    parts[part_count++] = pf; /* inner-first */
                                }
                                break;
                            }
                            recv = ts_node_child_by_field_name(recv, TS_FIELD("object"));
                        } else {
                            break;
                        }
                    }
                }
            }
        }
        cur = ts_node_parent(cur);
    }
    if (part_count == 0) {
        return NULL;
    }
    /* parts[] is inner-first; compose outer-first. Ensure exactly one '/'
     * between segments and a leading '/'. */
    char buf[CBM_SZ_256];
    size_t pos = 0;
    for (int i = part_count - 1; i >= 0; i--) {
        const char *seg = parts[i];
        while (*seg == '/') {
            seg++;
        }
        size_t sl = strlen(seg);
        if (sl == 0) {
            continue;
        }
        if (pos + sl + 2 >= sizeof(buf)) {
            return NULL; /* oversized — leave path un-prefixed */
        }
        buf[pos++] = '/';
        memcpy(buf + pos, seg, sl);
        pos += sl;
        while (pos > 1 && buf[pos - 1] == '/') {
            pos--; /* strip trailing slash per segment */
        }
    }
    buf[pos] = '\0';
    return pos ? cbm_arena_strndup(a, buf, pos) : NULL;
}

static char *extract_callee_name(CBMArena *a, TSNode node, const char *source, CBMLanguage lang) {
    // Lean 4: skip type-position applies
    if (lang == CBM_LANG_LEAN && strcmp(ts_node_type(node), "apply") == 0) {
        if (lean_is_in_type_position(node)) {
            return NULL;
        }
    }

    // Helm / Go templates: resolve `include "x"` / `template "x"` to the
    // referenced named template so it links to the define'd Function (#338).
    if (lang == CBM_LANG_GOTEMPLATE) {
        char *g = gotemplate_callee(a, node, source);
        if (g) {
            return g;
        }
    }

    // Constructor / instantiation nodes (new T(), object_creation, instance_expression):
    // resolve to the constructed type so a CALLS edge links to the class/constructor.
    char *ctor = extract_constructor_callee(a, node, source, ts_node_type(node));
    if (ctor) {
        return ctor;
    }

    // Ruby: `Widget.new(...)` is a method call on a constant receiver whose
    // method is `new`.  The constructor body lives in `initialize`, so a callee
    // of "new" never resolves.  Redirect to the receiver type name so the call
    // links to the class/constructor like every other language's `new T()`.
    if (lang == CBM_LANG_RUBY) {
        TSNode m = ts_node_child_by_field_name(node, TS_FIELD("method"));
        TSNode recv = ts_node_child_by_field_name(node, TS_FIELD("receiver"));
        if (!ts_node_is_null(m) && !ts_node_is_null(recv) &&
            strcmp(ts_node_type(recv), "constant") == 0) {
            char *mt = cbm_node_text(a, m, source);
            if (mt && strcmp(mt, "new") == 0) {
                char *rt = cbm_node_text(a, recv, source);
                if (rt && rt[0]) {
                    return rt;
                }
            }
        }
    }

    /* #952: PHP facade route registrations (`Route::get(...)`, a
     * scoped_call_expression) must carry the scope in the callee text — the
     * empty-resolution route fallback keys on the "::get" suffix table, and
     * the bare "get" that generic field resolution would return deliberately
     * never matches (every $obj->get() would become a route). Runs BEFORE
     * field-based resolution, which short-circuits on the `name` field.
     * Gated to the literal `Route` scope AND a route-method match: any other
     * scope (Cache::get) would suffix-match "::get" too and mint junk routes
     * from slash-prefixed keys. Known limitation: aliased facade imports are
     * not recognized. */
    if (lang == CBM_LANG_PHP && strcmp(ts_node_type(node), "scoped_call_expression") == 0) {
        TSNode scope = ts_node_child_by_field_name(node, TS_FIELD("scope"));
        TSNode mname = ts_node_child_by_field_name(node, TS_FIELD("name"));
        if (!ts_node_is_null(scope) && !ts_node_is_null(mname)) {
            char *sc = cbm_node_text(a, scope, source);
            char *mn = cbm_node_text(a, mname, source);
            if (sc && mn && strcmp(sc, "Route") == 0) {
                char *qual = cbm_arena_sprintf(a, "%s::%s", sc, mn);
                if (qual && cbm_service_pattern_route_method(qual) != NULL) {
                    return qual;
                }
            }
        }
    }

    // Try common field-based resolution first
    char *name = extract_callee_from_fields(a, node, source);
    if (name) {
        return name;
    }

    // Language-specific patterns
    name = extract_callee_lang_specific(a, node, source, lang);
    if (name) {
        return name;
    }

    // Generic fallback: first identifier child
    if (ts_node_child_count(node) > 0) {
        TSNode first = ts_node_child(node, 0);
        if (strcmp(ts_node_type(first), "identifier") == 0) {
            return cbm_node_text(a, first, source);
        }
    }

    return NULL;
}

// Strip quotes and validate a string arg. Returns validated text or NULL.
static const char *strip_and_validate_string_arg(CBMArena *a, char *text) {
    if (!text || !text[0]) {
        return NULL;
    }
    int len = (int)strlen(text);
    if (len >= CBM_QUOTE_PAIR && (text[0] == '"' || text[0] == '\'')) {
        text = cbm_arena_strndup(a, text + CBM_QUOTE_OFFSET, (size_t)(len - CBM_QUOTE_PAIR));
        len -= CBM_QUOTE_PAIR;
    }
    if (!text || len <= 0 || len >= MAX_STRING_ARG_LEN) {
        return NULL;
    }
    for (int vi = 0; vi < len; vi++) {
        if ((unsigned char)text[vi] < MIN_PRINTABLE && text[vi] != '\t') {
            return NULL;
        }
    }
    return text;
}

// Return the (dequoted) first string-literal child of a node, or NULL.
static char *gotemplate_string_child(CBMArena *a, TSNode parent, const char *source) {
    TSNode s = cbm_find_child_by_kind(parent, "interpreted_string_literal");
    if (ts_node_is_null(s)) {
        return NULL;
    }
    char *text = cbm_node_text(a, s, source);
    const char *v = strip_and_validate_string_arg(a, text);
    return (char *)v;
}

// Resolve a Go-template / Helm call to the referenced named template:
//   {{ template "x" . }}            -> template_action, name is a string child
//   {{ include "x" . }}             -> function_call(include), name is first string arg
// Returns NULL for any other node so generic resolution names the function.
static char *gotemplate_callee(CBMArena *a, TSNode node, const char *source) {
    const char *k = ts_node_type(node);
    if (strcmp(k, "template_action") == 0) {
        return gotemplate_string_child(a, node, source);
    }
    if (strcmp(k, "function_call") == 0) {
        TSNode fn = cbm_find_child_by_kind(node, "identifier");
        if (ts_node_is_null(fn)) {
            return NULL;
        }
        char *fname = cbm_node_text(a, fn, source);
        if (!fname || (strcmp(fname, "include") != 0 && strcmp(fname, "template") != 0 &&
                       strcmp(fname, "tpl") != 0)) {
            return NULL;
        }
        TSNode args = ts_node_child_by_field_name(node, TS_FIELD("arguments"));
        if (ts_node_is_null(args)) {
            args = cbm_find_child_by_kind(node, "argument_list");
        }
        if (ts_node_is_null(args)) {
            return NULL;
        }
        return gotemplate_string_child(a, args, source);
    }
    return NULL;
}

static const char *extract_nth_string_arg(CBMExtractCtx *ctx, TSNode args, uint32_t n) {
    uint32_t nc = ts_node_named_child_count(args);
    uint32_t found = 0;
    for (uint32_t ai = 0; ai < nc && ai < MAX_POSITIONAL_SCAN + n; ai++) {
        TSNode arg = ts_node_named_child(args, ai);
        const char *ak = ts_node_type(arg);
        if (is_string_like(ak)) {
            if (found == n) {
                char *text = cbm_node_text(ctx->arena, arg, ctx->source);
                return strip_and_validate_string_arg(ctx->arena, text);
            }
            found++;
        }
    }
    return NULL;
}

// --- Unified handler: called once per node by the cursor walk ---

// Process a keyword argument (keyword_argument or pair node).
static void process_keyword_arg(CBMExtractCtx *ctx, TSNode arg_node, CBMCallArg *ca) {
    TSNode key_n = ts_node_child_by_field_name(arg_node, TS_FIELD("name"));
    TSNode val_n = ts_node_child_by_field_name(arg_node, TS_FIELD("value"));
    if (ts_node_is_null(key_n)) {
        key_n = ts_node_child_by_field_name(arg_node, TS_FIELD("key"));
    }
    if (!ts_node_is_null(key_n)) {
        ca->keyword = cbm_node_text(ctx->arena, key_n, ctx->source);
    }
    if (!ts_node_is_null(val_n)) {
        ca->expr = cbm_node_text(ctx->arena, val_n, ctx->source);
        ca->value = cbm_evaluate_static_string(ctx, val_n, 0);
    }
}

static const char *normalize_http_method(CBMExtractCtx *ctx, const char *value) {
    if (!value || !value[0]) {
        return NULL;
    }
    static const char *methods[] = {"GET",   "POST", "PUT",     "DELETE",
                                    "PATCH", "HEAD", "OPTIONS", NULL};
    char upper[CBM_SZ_32];
    size_t n = strlen(value);
    if (n == 0 || n >= sizeof(upper)) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        upper[i] = (char)toupper((unsigned char)value[i]);
    }
    upper[n] = '\0';
    for (int i = 0; methods[i]; i++) {
        if (strcmp(upper, methods[i]) == 0) {
            return cbm_arena_strndup(ctx->arena, upper, n);
        }
    }
    return NULL;
}

/* Extract all arguments from a call expression into call->args[]. */
static void extract_call_args(CBMExtractCtx *ctx, TSNode args, CBMCall *call) {
    uint32_t argc = ts_node_named_child_count(args);
    int positional_idx = 0;
    for (uint32_t ai = 0; ai < argc && call->arg_count < CBM_MAX_CALL_ARGS; ai++) {
        TSNode arg_node = ts_node_named_child(args, ai);
        const char *ak = ts_node_type(arg_node);
        CBMCallArg *ca = &call->args[call->arg_count];
        memset(ca, 0, sizeof(*ca));

        if (strcmp(ak, "keyword_argument") == 0 || strcmp(ak, "pair") == 0) {
            process_keyword_arg(ctx, arg_node, ca);
            if (ca->keyword && strcmp(ca->keyword, "method") == 0) {
                call->http_method = normalize_http_method(ctx, ca->value);
            }
            ca->index = positional_idx++;
            call->arg_count++;
        } else if (strcmp(ak, "list_splat") == 0 || strcmp(ak, "dictionary_splat") == 0 ||
                   strcmp(ak, "spread_element") == 0) {
            positional_idx++;
        } else {
            ca->expr = cbm_node_text(ctx->arena, arg_node, ctx->source);
            ca->index = positional_idx++;
            ca->value = cbm_evaluate_static_string(ctx, arg_node, 0);
            call->arg_count++;
        }
    }
}

// Check if a keyword name matches URL or topic patterns.
static bool is_url_or_topic_keyword(const char *key) {
    static const char *url_keywords[] = {"url",        "endpoint", "path", "uri",
                                         "target_url", "base_url", NULL};
    static const char *topic_keywords[] = {"topic",   "topic_id",   "topic_name",
                                           "queue",   "queue_name", "queue_id",
                                           "subject", "channel",    NULL};
    for (int i = 0; url_keywords[i]; i++) {
        if (strcmp(key, url_keywords[i]) == 0) {
            return true;
        }
    }
    for (int i = 0; topic_keywords[i]; i++) {
        if (strcmp(key, topic_keywords[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Check if a struct-field name identifies a queue/topic target.  Cloud SDKs pass
// the destination via a composite-literal input struct rather than a bare string
// arg (e.g. Go `SendMessageInput{QueueUrl: ...}`, `PublishInput{TopicArn: ...}`).
// Case-insensitive so QueueUrl/QueueURL/queue_url all match.
static bool is_queue_topic_field(const char *key) {
    static const char *fields[] = {"QueueUrl",  "QueueURL", "TopicArn", "TopicARN",    "QueueName",
                                   "TopicName", "QueueArn", "QueueARN", "Destination", NULL};
    if (!key || !key[0]) {
        return false;
    }
    for (int i = 0; fields[i]; i++) {
        if (strcasecmp(key, fields[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Extract string value from a node (literal or constant reference).
static const char *extract_string_value(CBMExtractCtx *ctx, TSNode val_node) {
    const char *evaluated = cbm_evaluate_static_string(ctx, val_node, 0);
    if (evaluated) {
        return evaluated;
    }
    const char *vk = ts_node_type(val_node);
    if (is_string_like(vk)) {
        char *text = cbm_node_text(ctx->arena, val_node, ctx->source);
        if (text && text[0]) {
            return strip_quotes(ctx->arena, text);
        }
    } else if (strcmp(vk, "identifier") == 0) {
        char *const_name = cbm_node_text(ctx->arena, val_node, ctx->source);
        if (const_name) {
            return lookup_string_constant(ctx, const_name);
        }
    }
    return NULL;
}

// Recover a queue/topic identity from a Go composite-literal input struct, e.g.
//   &sqs.SendMessageInput{QueueUrl: queueUrl, MessageBody: body}
//   sns.PublishInput{TopicArn: "arn:aws:sns:..."}
// The dispatch target is carried by a struct field (QueueUrl/TopicArn/...), not a
// bare string arg, so the async edge would otherwise degrade to a plain CALLS.
// Returns the field's value: the string-literal content when present, else the
// referenced identifier text (which still names the queue/topic for edge formation).
static const char *extract_composite_queue_field(CBMExtractCtx *ctx, TSNode node) {
    // Unwrap a pointer-of-composite: `&Type{...}` is a unary_expression whose
    // operand is the composite_literal.
    if (strcmp(ts_node_type(node), "unary_expression") == 0) {
        TSNode operand = ts_node_child_by_field_name(node, TS_FIELD("operand"));
        if (ts_node_is_null(operand)) {
            return NULL;
        }
        node = operand;
    }
    if (strcmp(ts_node_type(node), "composite_literal") != 0) {
        return NULL;
    }
    TSNode body = ts_node_child_by_field_name(node, TS_FIELD("body"));
    if (ts_node_is_null(body)) {
        body = cbm_find_child_by_kind(node, "literal_value");
    }
    if (ts_node_is_null(body)) {
        return NULL;
    }
    uint32_t nc = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < nc; i++) {
        TSNode el = ts_node_named_child(body, i);
        if (strcmp(ts_node_type(el), "keyed_element") != 0) {
            continue;
        }
        // keyed_element children: key then value. Each side may be wrapped in a
        // literal_element; unwrap to the underlying identifier/literal.
        uint32_t ec = ts_node_named_child_count(el);
        if (ec < PAIR_LEN) {
            continue;
        }
        TSNode key_n = ts_node_named_child(el, 0);
        TSNode val_n = ts_node_named_child(el, 1);
        if (strcmp(ts_node_type(key_n), "literal_element") == 0 &&
            ts_node_named_child_count(key_n) > 0) {
            key_n = ts_node_named_child(key_n, 0);
        }
        if (strcmp(ts_node_type(val_n), "literal_element") == 0 &&
            ts_node_named_child_count(val_n) > 0) {
            val_n = ts_node_named_child(val_n, 0);
        }
        char *key = cbm_node_text(ctx->arena, key_n, ctx->source);
        if (!is_queue_topic_field(key)) {
            continue;
        }
        const char *resolved = extract_string_value(ctx, val_n);
        if (resolved && resolved[0]) {
            return resolved;
        }
        // Value is a variable/expression (no constant value); use its source text
        // as the queue/topic identity so the async edge still forms.
        char *raw = cbm_node_text(ctx->arena, val_n, ctx->source);
        if (raw && raw[0]) {
            return raw;
        }
    }
    return NULL;
}

// Try to extract URL/topic from a keyword_argument or pair node.
static const char *extract_keyword_url(CBMExtractCtx *ctx, TSNode arg) {
    TSNode key_node = ts_node_child_by_field_name(arg, TS_FIELD("name"));
    TSNode val_node = ts_node_child_by_field_name(arg, TS_FIELD("value"));
    if (ts_node_is_null(key_node)) {
        key_node = ts_node_child_by_field_name(arg, TS_FIELD("key"));
    }
    if (ts_node_is_null(key_node) || ts_node_is_null(val_node)) {
        return NULL;
    }
    char *key = cbm_node_text(ctx->arena, key_node, ctx->source);
    if (!key || !is_url_or_topic_keyword(key)) {
        return NULL;
    }
    return extract_string_value(ctx, val_node);
}

// Try to extract URL/topic from a positional argument (string or constant).
static const char *extract_positional_url(CBMExtractCtx *ctx, TSNode arg, const char *ak) {
    const char *evaluated = cbm_evaluate_static_string(ctx, arg, 0);
    if (evaluated) {
        return evaluated;
    }
    if (is_string_like(ak)) {
        char *text = cbm_node_text(ctx->arena, arg, ctx->source);
        const char *validated = strip_and_validate_string_arg(ctx->arena, text);
        if (validated) {
            return validated;
        }
    }
    if (strcmp(ak, "identifier") == 0) {
        char *const_name = cbm_node_text(ctx->arena, arg, ctx->source);
        if (const_name) {
            return lookup_string_constant(ctx, const_name);
        }
    }
    return NULL;
}

// Extract URL/topic candidates from keyword or positional args. JavaScript
// conditionals and dynamic object-map lookups may conservatively yield more
// than one statically valid endpoint.
static int extract_url_or_topic_args(CBMExtractCtx *ctx, TSNode args, StaticStringCandidates *out) {
    uint32_t nc = ts_node_named_child_count(args);
    for (uint32_t ai = 0; ai < nc; ai++) {
        TSNode arg = ts_node_named_child(args, ai);
        /* PHP and C# wrap each positional argument in an `argument` node;
         * unwrap to the underlying value so the URL string is reachable. */
        if (strcmp(ts_node_type(arg), "argument") == 0 && ts_node_named_child_count(arg) > 0) {
            arg = ts_node_named_child(arg, 0);
        }
        const char *ak = ts_node_type(arg);

        if (strcmp(ak, "keyword_argument") == 0 || strcmp(ak, "pair") == 0) {
            TSNode key_node = ts_node_child_by_field_name(arg, TS_FIELD("name"));
            TSNode value_node = ts_node_child_by_field_name(arg, TS_FIELD("value"));
            if (ts_node_is_null(key_node)) {
                key_node = ts_node_child_by_field_name(arg, TS_FIELD("key"));
            }
            char *key =
                ts_node_is_null(key_node) ? NULL : cbm_node_text(ctx->arena, key_node, ctx->source);
            if (key && is_url_or_topic_keyword(key) && !ts_node_is_null(value_node)) {
                collect_static_string_candidates(ctx, value_node, value_node, 0, out);
                if (out->count > 0) {
                    return out->count;
                }
            }
            const char *val = extract_keyword_url(ctx, arg);
            if (val) {
                static_candidate_add(out, val);
                return out->count;
            }
            continue;
        }

        /* Cloud SDK dispatch via input struct: the queue/topic target is a field
         * of a composite literal (Go `&sqs.SendMessageInput{QueueUrl: ...}`), not
         * a bare string arg. Recover it so the async edge forms. */
        if (strcmp(ak, "composite_literal") == 0 || strcmp(ak, "unary_expression") == 0) {
            const char *val = extract_composite_queue_field(ctx, arg);
            if (val) {
                static_candidate_add(out, val);
                return out->count;
            }
        }

        if (ai < MAX_POSITIONAL_SCAN) {
            collect_static_string_candidates(ctx, arg, arg, 0, out);
            if (out->count > 0) {
                return out->count;
            }
            const char *val = extract_positional_url(ctx, arg, ak);
            if (val) {
                static_candidate_add(out, val);
                return out->count;
            }
        }
    }
    return out->count;
}

static void call_set_url_candidate(CBMCall *call, const char *previous, const char *candidate) {
    call->first_string_arg = candidate;
    for (int i = 0; i < call->arg_count; i++) {
        CBMCallArg *arg = &call->args[i];
        bool same_value = previous && arg->value && strcmp(arg->value, previous) == 0;
        bool url_keyword = arg->keyword && is_url_or_topic_keyword(arg->keyword);
        if (same_value || url_keyword || arg->index == 0) {
            arg->value = candidate;
            return;
        }
    }
}

// Extract second argument name (handler ref for route registrations).
/* Normalize a string-form route handler to a resolvable handler name.
 *   'showUsers'              → showUsers
 *   'UserController@show'    → show   (Laravel "Controller@method")
 * The method segment after '@' is the resolvable function/method name. */
static const char *normalize_string_handler(CBMArena *a, const char *raw) {
    const char *unq = strip_quotes(a, raw);
    if (!unq || !unq[0]) {
        return NULL;
    }
    const char *at = strchr(unq, '@');
    if (at && at[1]) {
        return cbm_arena_strdup(a, at + 1);
    }
    return unq;
}

static const char *extract_handler_arg(CBMExtractCtx *ctx, TSNode args) {
    uint32_t nc = ts_node_named_child_count(args);
    for (uint32_t ai = HANDLER_START_IDX; ai < nc && ai < MAX_HANDLER_SCAN; ai++) {
        TSNode arg2 = ts_node_named_child(args, ai);
        /* PHP wraps each argument in an `argument` node — unwrap to the value. */
        if (strcmp(ts_node_type(arg2), "argument") == 0 && ts_node_named_child_count(arg2) > 0) {
            arg2 = ts_node_named_child(arg2, 0);
        }
        const char *ak2 = ts_node_type(arg2);
        /* `name` = PHP bare identifier handler; string = Laravel string handler
         * ('showUsers' or 'Controller@method'). */
        if (strcmp(ak2, "identifier") == 0 || strcmp(ak2, "member_expression") == 0 ||
            strcmp(ak2, "selector_expression") == 0 || strcmp(ak2, "attribute") == 0 ||
            strcmp(ak2, "field_expression") == 0 || strcmp(ak2, "name") == 0) {
            return cbm_node_text(ctx->arena, arg2, ctx->source);
        }
        if (is_string_like(ak2)) {
            const char *h =
                normalize_string_handler(ctx->arena, cbm_node_text(ctx->arena, arg2, ctx->source));
            if (h && h[0]) {
                return h;
            }
        }
    }
    return NULL;
}

/* FastAPI supports registering an existing callable without decorator syntax:
 *
 *
 * app.post("/items")(create_item)
 *
 * Tree-sitter represents this as an outer call whose function
 * is the inner
 * app.post("/items") call.  The regular walk records the inner route call but
 *
 * cannot see the handler supplied to the outer call, leaving the Route without
 * a HANDLES edge.
 * Fold the two calls into one additional registration record. */
static void extract_python_chained_route_registration(CBMExtractCtx *ctx, TSNode node,
                                                      WalkState *state) {
    if (ctx->language != CBM_LANG_PYTHON || strcmp(ts_node_type(node), "call") != 0) {
        return;
    }

    TSNode registrar = ts_node_child_by_field_name(node, TS_FIELD("function"));
    if (ts_node_is_null(registrar) || strcmp(ts_node_type(registrar), "call") != 0) {
        return;
    }
    char *callee = extract_callee_name(ctx->arena, registrar, ctx->source, ctx->language);
    if (!callee || !cbm_service_pattern_route_method(callee)) {
        return;
    }

    TSNode route_args = ts_node_child_by_field_name(registrar, TS_FIELD("arguments"));
    TSNode handler_args = ts_node_child_by_field_name(node, TS_FIELD("arguments"));
    if (ts_node_is_null(route_args) || ts_node_is_null(handler_args) ||
        ts_node_named_child_count(handler_args) == 0) {
        return;
    }

    StaticStringCandidates candidates = {0};
    extract_url_or_topic_args(ctx, route_args, &candidates);
    if (candidates.count == 0 || !candidates.values[0] || candidates.values[0][0] != '/') {
        return;
    }

    TSNode handler_node = ts_node_named_child(handler_args, 0);
    const char *handler_kind = ts_node_type(handler_node);
    if (strcmp(handler_kind, "identifier") != 0 && strcmp(handler_kind, "attribute") != 0) {
        return;
    }
    char *handler = cbm_node_text(ctx->arena, handler_node, ctx->source);
    if (!handler || !handler[0]) {
        return;
    }

    CBMCall call = {0};
    call.callee_name = callee;
    call.enclosing_func_qn = state->enclosing_func_qn;
    call.first_string_arg = candidates.values[0];
    call.second_arg_name = handler;
    call.is_route_registration = true;
    call.loop_depth = state->loop_depth;
    call.branch_depth = state->branch_depth;
    call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
    extract_call_args(ctx, route_args, &call);
    cbm_calls_push(&ctx->result->calls, ctx->arena, call);
}

// Extract JSX component refs (uppercase tags) as CALLS edges.
static void extract_jsx_component_ref(CBMExtractCtx *ctx, TSNode node, const char *kind,
                                      const char *enclosing_func_qn) {
    if (strcmp(kind, "jsx_self_closing_element") != 0 && strcmp(kind, "jsx_opening_element") != 0) {
        return;
    }
    TSNode name_node = ts_node_child_by_field_name(node, TS_FIELD("name"));
    if (ts_node_is_null(name_node)) {
        return;
    }
    char *name = cbm_node_text(ctx->arena, name_node, ctx->source);
    if (name && name[0] >= 'A' && name[0] <= 'Z') {
        CBMCall call = {0};
        call.callee_name = name;
        call.enclosing_func_qn = enclosing_func_qn;
        cbm_calls_push(&ctx->result->calls, ctx->arena, call);
    }
}

// Kotlin: `a OP b` desugars to an operator-method call `a.<method>(b)`. The
// generic call walk keys on call_expression nodes and so never sees these
// precedence-specific binary-expression nodes, leaving the type-aware LSP
// operator resolution (lsp_kt_operator -> the user `operator fun`) with no call
// site to attach to. Record a textual call to the operator method's bare name;
// the operator-token -> method mapping mirrors kotlin_lsp.c's binary handler so
// the names join. Builtin operands (Int+Int) resolve to a stdlib type with no
// graph node and drop, exactly as before — only user `operator fun`s gain edges.
static void extract_kotlin_operator_call(CBMExtractCtx *ctx, TSNode node, const char *kind,
                                         const char *enclosing_func_qn) {
    if (strcmp(kind, "binary_expression") != 0 && strcmp(kind, "additive_expression") != 0 &&
        strcmp(kind, "multiplicative_expression") != 0 &&
        strcmp(kind, "comparison_expression") != 0 && strcmp(kind, "equality_expression") != 0 &&
        strcmp(kind, "range_expression") != 0) {
        return;
    }
    uint32_t ncc = ts_node_named_child_count(node);
    TSNode lhs = ts_node_child_by_field_name(node, TS_FIELD("left"));
    TSNode rhs = ts_node_child_by_field_name(node, TS_FIELD("right"));
    if (ts_node_is_null(lhs) && ncc >= 1) {
        lhs = ts_node_named_child(node, 0);
    }
    if (ts_node_is_null(rhs) && ncc >= 2) {
        rhs = ts_node_named_child(node, ncc - 1);
    }
    if (ts_node_is_null(lhs) || ts_node_is_null(rhs)) {
        return;
    }
    uint32_t lhs_end = ts_node_end_byte(lhs);
    uint32_t rhs_start = ts_node_start_byte(rhs);
    if (rhs_start <= lhs_end) {
        return;
    }
    const char *between = ctx->source + lhs_end;
    size_t blen = (size_t)(rhs_start - lhs_end);
    const char *op_method = NULL;
    if (cbm_memmem(between, blen, "===", 3) || cbm_memmem(between, blen, "!==", 3)) {
        return; // identity comparison: no operator method
    } else if (cbm_memmem(between, blen, "==", 2) || cbm_memmem(between, blen, "!=", 2)) {
        op_method = "equals";
    } else if (cbm_memmem(between, blen, "..<", 3)) {
        op_method = "rangeUntil";
    } else if (cbm_memmem(between, blen, "..", 2)) {
        op_method = "rangeTo";
    } else if (cbm_memmem(between, blen, "<", 1) || cbm_memmem(between, blen, ">", 1)) {
        op_method = "compareTo"; // covers <, >, <=, >=
    } else if (cbm_memmem(between, blen, "+", 1)) {
        op_method = "plus";
    } else if (cbm_memmem(between, blen, "-", 1)) {
        op_method = "minus";
    } else if (cbm_memmem(between, blen, "*", 1)) {
        op_method = "times";
    } else if (cbm_memmem(between, blen, "/", 1)) {
        op_method = "div";
    } else if (cbm_memmem(between, blen, "%", 1)) {
        op_method = "rem";
    }
    if (!op_method) {
        return;
    }
    CBMCall call = {0};
    call.callee_name = op_method;
    call.enclosing_func_qn = enclosing_func_qn;
    call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
    cbm_calls_push(&ctx->result->calls, ctx->arena, call);
}

// Kotlin convention-desugared calls that the call walk never sees as
// call_expressions: `val (a,b) = e` -> e.component1()/e.component2(); and
// `for (x in e)` -> e.iterator()/hasNext()/next(). Record textual calls to those
// operator-convention method names so the LSP's lsp_kt_destructure /
// lsp_kt_iterator resolutions have a call site to join (names match the LSP's).
static void kt_push_implicit_call(CBMExtractCtx *ctx, TSNode node, const char *callee,
                                  const char *enclosing_func_qn) {
    CBMCall call = {0};
    call.callee_name = callee;
    call.enclosing_func_qn = enclosing_func_qn;
    call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
    cbm_calls_push(&ctx->result->calls, ctx->arena, call);
}

// C++ overloaded binary operator `a + b`: the operator method (`operator+`) is
// invoked implicitly, so the call walk never sees a call node. Synthesize a
// textual call to the bare operator name so the c-LSP's lsp_operator resolution
// (which keys the same `operator<tok>` member on the lhs type) has a call site to
// join. The operator token is the first unnamed child, mirroring c_lsp.c's binary
// handling. Builtin-operand expressions (int + int) synthesize an `operator+`
// callee too, but no such member exists so the call resolves to nothing and is
// dropped — no spurious edge.
static void extract_cpp_operator_call(CBMExtractCtx *ctx, TSNode node, const char *kind,
                                      const char *enclosing_func_qn) {
    if (strcmp(kind, "binary_expression") != 0) {
        return;
    }
    TSNode lhs = ts_node_child_by_field_name(node, TS_FIELD("left"));
    TSNode rhs = ts_node_child_by_field_name(node, TS_FIELD("right"));
    if (ts_node_is_null(lhs) || ts_node_is_null(rhs)) {
        return;
    }
    for (uint32_t i = 0; i < ts_node_child_count(node); i++) {
        TSNode child = ts_node_child(node, i);
        if (ts_node_is_named(child)) {
            continue;
        }
        char *op = cbm_node_text(ctx->arena, child, ctx->source);
        if (op && op[0]) {
            CBMCall call = {0};
            call.callee_name = cbm_arena_sprintf(ctx->arena, "operator%s", op);
            call.enclosing_func_qn = enclosing_func_qn;
            call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
            cbm_calls_push(&ctx->result->calls, ctx->arena, call);
        }
        break;
    }
}

// C++ implicit calls that produce no textual call node: the destructor
// (`delete p`), the copy/move constructor (`T a = b;` copy-init), and the
// conversion operator (`if (obj)` where obj has `operator bool`). The c-LSP
// resolves each to the corresponding member but there is no call site to join
// to (callable=0). Synthesize a textual call sourced to the enclosing function
// so the lsp_{destructor,copy_constructor,conversion} resolution binds.
//
//   - destructor: the callee QN embeds the type (`T.~T`), which is not textually
//     available from `delete p`, so it joins via the reason gate — c_lsp stashes
//     the operand text in `reason` and the synthesized callee is that same text.
//   - copy constructor: the callee short-name is the constructed type (`T`),
//     which IS textually present as the declaration's type — join by short-name.
//   - conversion: the callee short-name is the type-independent `operator bool`.
//
// Spurious synthesis (a condition/operand that has no such member) resolves to
// nothing and is dropped, so no extra edge is produced.
static void extract_cpp_implicit_calls(CBMExtractCtx *ctx, TSNode node, const char *kind,
                                       const char *enclosing_func_qn) {
    const char *callee = NULL;
    if (strcmp(kind, "delete_expression") == 0) {
        TSNode operand = ts_node_child_by_field_name(node, TS_FIELD("argument"));
        if (ts_node_is_null(operand) && ts_node_named_child_count(node) > 0) {
            operand = ts_node_named_child(node, 0);
        }
        if (!ts_node_is_null(operand)) {
            callee = cbm_node_text(ctx->arena, operand, ctx->source);
        }
    } else if (strcmp(kind, "if_statement") == 0 || strcmp(kind, "while_statement") == 0 ||
               strcmp(kind, "do_statement") == 0) {
        // `if (obj)` invokes obj's `operator bool`. Only a lone-identifier
        // condition triggers it; comparisons/logical exprs evaluate to bool.
        TSNode cond = ts_node_child_by_field_name(node, TS_FIELD("condition"));
        if (!ts_node_is_null(cond)) {
            TSNode inner = cond;
            if (strcmp(ts_node_type(cond), "condition_clause") == 0 &&
                ts_node_named_child_count(cond) == 1) {
                inner = ts_node_named_child(cond, 0);
            }
            if (strcmp(ts_node_type(inner), "identifier") == 0) {
                callee = "operator bool";
            }
        }
    } else if (strcmp(kind, "declaration") == 0) {
        // `T a = b;` — copy-init from an identifier invokes T's copy constructor.
        TSNode type = ts_node_child_by_field_name(node, TS_FIELD("type"));
        TSNode decl = ts_node_child_by_field_name(node, TS_FIELD("declarator"));
        if (!ts_node_is_null(type) && !ts_node_is_null(decl) &&
            strcmp(ts_node_type(decl), "init_declarator") == 0) {
            TSNode value = ts_node_child_by_field_name(decl, TS_FIELD("value"));
            if (!ts_node_is_null(value) && strcmp(ts_node_type(value), "identifier") == 0) {
                char *tn = cbm_node_text(ctx->arena, type, ctx->source);
                if (tn) {
                    const char *colon = strrchr(tn, ':');
                    callee = colon ? colon + 1 : tn;
                }
            }
        }
    }
    if (callee && callee[0]) {
        CBMCall call = {0};
        call.callee_name = callee;
        call.enclosing_func_qn = enclosing_func_qn;
        call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
        cbm_calls_push(&ctx->result->calls, ctx->arena, call);
    }
}

static void extract_kotlin_desugared_calls(CBMExtractCtx *ctx, TSNode node, const char *kind,
                                           const char *enclosing_func_qn) {
    if (strcmp(kind, "property_declaration") == 0) {
        uint32_t nc = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_named_child(node, i);
            if (strcmp(ts_node_type(c), "multi_variable_declaration") != 0) {
                continue;
            }
            // One componentN() call per destructured variable.
            uint32_t vc = ts_node_named_child_count(c);
            uint32_t comp = 0;
            for (uint32_t j = 0; j < vc; j++) {
                TSNode v = ts_node_named_child(c, j);
                if (strcmp(ts_node_type(v), "variable_declaration") != 0) {
                    continue;
                }
                comp++;
                kt_push_implicit_call(ctx, node, cbm_arena_sprintf(ctx->arena, "component%u", comp),
                                      enclosing_func_qn);
            }
            break;
        }
    } else if (strcmp(kind, "for_statement") == 0) {
        kt_push_implicit_call(ctx, node, "iterator", enclosing_func_qn);
        kt_push_implicit_call(ctx, node, "hasNext", enclosing_func_qn);
        kt_push_implicit_call(ctx, node, "next", enclosing_func_qn);
    }
}

// Java method reference `Lhs::name` (e.g. `String::length`, `Foo::new`). The
// call walk only visits call_expression-like nodes, so a method_reference never
// becomes a call and the LSP's lsp_method_ref resolution has no call site to
// attach to. Record a textual call to the referenced method's bare name (the
// constructor ref `Lhs::new` uses the unnamed `new` token); the LSP join then
// matches on the bare name. The referenced method IS invoked indirectly, so
// this is an accurate call edge (mirrors java_lsp.c resolve_method_reference).
static void extract_java_method_reference(CBMExtractCtx *ctx, TSNode node, const char *kind,
                                          const char *enclosing_func_qn) {
    if (strcmp(kind, "method_reference") != 0) {
        return;
    }
    uint32_t nc = ts_node_named_child_count(node);
    if (nc < 1) {
        return;
    }
    char *mname = NULL;
    if (nc >= 2) {
        mname = cbm_node_text(ctx->arena, ts_node_named_child(node, nc - 1), ctx->source);
    }
    if (!mname || !mname[0]) {
        mname = "new"; // constructor reference `Lhs::new` — `new` is unnamed
    }
    CBMCall call = {0};
    call.callee_name = mname;
    call.enclosing_func_qn = enclosing_func_qn;
    call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
    cbm_calls_push(&ctx->result->calls, ctx->arena, call);
}

// ObjectScript: resolve `var.Method(...)` / `..Property.Method(...)` instance
// calls against the per-method variable type map. Returns arena "Class.Method"
// or NULL if the receiver's type is unknown.
static char *resolve_objectscript_instance_call(CBMArena *a, TSNode node, const char *source,
                                                os_type_map_t *type_map) {
    TSNode receiver = {0};
    TSNode oref = {0};
    const char *nk_first = NULL;
    for (uint32_t i = 0; i < ts_node_named_child_count(node); i++) {
        TSNode child = ts_node_named_child(node, i);
        const char *ck = ts_node_type(child);
        if (strcmp(ck, "lvn") == 0 || strcmp(ck, "variable") == 0) {
            receiver = child;
        } else if (strcmp(ck, "relative_dot_property") == 0) {
            receiver = child;
            nk_first = "relative_dot_property";
        } else if (strcmp(ck, "oref_method") == 0) {
            oref = child;
        }
    }
    if (ts_node_is_null(oref)) {
        return NULL;
    }
    TSNode method_name_node = cbm_find_child_by_kind(oref, "method_name");
    if (ts_node_is_null(method_name_node)) {
        return NULL;
    }
    TSNode mn_ident = ts_node_named_child_count(method_name_node) > 0
                          ? ts_node_named_child(method_name_node, 0)
                          : (TSNode){0};
    if (ts_node_is_null(mn_ident)) {
        return NULL;
    }
    char *method = cbm_node_text(a, mn_ident, source);
    if (!method || !method[0]) {
        return NULL;
    }
    if (ts_node_is_null(receiver)) {
        return NULL;
    }
    char *var_text = NULL;
    if (nk_first && strcmp(nk_first, "relative_dot_property") == 0) {
        TSNode prop_name = cbm_find_child_by_kind(receiver, "member_name");
        if (!ts_node_is_null(prop_name)) {
            char *pname = cbm_node_text(a, prop_name, source);
            if (pname && pname[0]) {
                var_text = cbm_arena_sprintf(a, "..%s", pname);
            }
        }
        if (!var_text) {
            var_text = cbm_node_text(a, receiver, source);
        }
    } else {
        var_text = cbm_node_text(a, receiver, source);
    }
    if (!var_text || !var_text[0]) {
        return NULL;
    }
    for (int i = 0; i < type_map->count; i++) {
        if (strcasecmp(type_map->entries[i].var_name, var_text) == 0) {
            return cbm_arena_sprintf(a, "%s.%s", type_map->entries[i].class_name, method);
        }
    }
    return NULL;
}

void handle_calls(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state) {
    if (!spec->call_node_types || !spec->call_node_types[0]) {
        return;
    }

    if (cbm_kind_in_set(node, spec->call_node_types)) {
        extract_python_chained_route_registration(ctx, node, state);
        char *callee = extract_callee_name(ctx->arena, node, ctx->source, ctx->language);

        // ObjectScript: var.Method() / ..Property.Method() instance dispatch.
        if (!callee &&
            (ctx->language == CBM_LANG_OBJECTSCRIPT_UDL ||
             ctx->language == CBM_LANG_OBJECTSCRIPT_ROUTINE) &&
            strcmp(ts_node_type(node), "method_call") == 0) {
            callee = resolve_objectscript_instance_call(ctx->arena, node, ctx->source,
                                                        &state->os_type_map);
        }

        // ObjectScript: ..Method() oref self-call resolves against the enclosing class.
        if (!callee &&
            (ctx->language == CBM_LANG_OBJECTSCRIPT_UDL ||
             ctx->language == CBM_LANG_OBJECTSCRIPT_ROUTINE) &&
            strcmp(ts_node_type(node), "relative_dot_method") == 0 && state->enclosing_class_qn &&
            state->enclosing_class_qn[0]) {
            TSNode oref = cbm_find_child_by_kind(node, "oref_method");
            if (!ts_node_is_null(oref)) {
                TSNode mname_node = cbm_find_child_by_kind(oref, "method_name");
                if (!ts_node_is_null(mname_node)) {
                    TSNode ident = ts_node_named_child_count(mname_node) > 0
                                       ? ts_node_named_child(mname_node, 0)
                                       : (TSNode){0};
                    if (!ts_node_is_null(ident)) {
                        char *mname = cbm_node_text(ctx->arena, ident, ctx->source);
                        if (mname && mname[0]) {
                            callee = cbm_arena_sprintf(ctx->arena, "%s.%s",
                                                       state->enclosing_class_qn, mname);
                        }
                    }
                }
            }
        }

        // ObjectScript: expand a $$$Macro callee via the macro table.
        if (callee && callee[0] == '$' && callee[1] == '$' && callee[2] == '$' &&
            ctx->macro_table) {
            const char *macro_name = callee + 3;
            const CBMMacroEntry *entry = cbm_macro_table_find(ctx->macro_table, macro_name);
            if (entry) {
                if (entry->resolved_callee) {
                    callee = cbm_arena_strdup(ctx->arena, entry->resolved_callee);
                } else if (entry->expansion) {
                    callee = cbm_macro_extract_callee(ctx->arena, entry->expansion);
                } else {
                    callee = NULL;
                }
            }
        }

        // Keyword-filter callees, but keep builtins we mint a node for (len, str,
        // ...) so the LSP-resolved builtin call still forms a CALLS edge.
        if (callee && callee[0] &&
            (!cbm_is_keyword(callee, ctx->language) ||
             cbm_is_resolvable_builtin(callee, ctx->language))) {
            CBMCall call = {0};
            StaticStringCandidates url_candidates = {0};
            call.callee_name = callee;
            call.enclosing_func_qn = state->enclosing_func_qn;
            call.loop_depth = state->loop_depth;     // enclosing loop nesting at this call
            call.branch_depth = state->branch_depth; // enclosing branch nesting at this call
            call.start_line = (int)ts_node_start_point(node).row + TS_LINE_OFFSET;
            // Perl-only: flag arrow/method calls ($obj->m / Class->m). The
            // generic short-name resolver cannot place a method without a known
            // receiver type, so the call-resolution pass suppresses those edges.
            // Default false for every other language (struct is zero-init).
            if (ctx->language == CBM_LANG_PERL &&
                strcmp(ts_node_type(node), "method_call_expression") == 0) {
                call.is_method = true;
            }
            // TS/JS/TSX receiver-aware guard (#592/#606 direction; same intent
            // as the Perl flag above). Flag a member call x.foo() whose receiver
            // is NOT `this`/`super`. When the TS-LSP cannot resolve the receiver
            // type, the call-resolution pass suppresses weak short-name matches
            // for these (so `re.test()` cannot fabricate an edge to a project
            // `test`). this/super receivers stay unflagged — their target is the
            // enclosing class, where a namespace-proximity weak match is usually
            // right. Bare calls (helper()) and new_expression have no member
            // receiver, so they keep is_method=false (struct is zero-init).
            if ((ctx->language == CBM_LANG_JAVASCRIPT || ctx->language == CBM_LANG_TYPESCRIPT ||
                 ctx->language == CBM_LANG_TSX) &&
                strcmp(ts_node_type(node), "call_expression") == 0) {
                TSNode fn = ts_node_child_by_field_name(node, TS_FIELD("function"));
                if (!ts_node_is_null(fn) && strcmp(ts_node_type(fn), "member_expression") == 0) {
                    TSNode obj = ts_node_child_by_field_name(fn, TS_FIELD("object"));
                    if (!ts_node_is_null(obj)) {
                        const char *ok = ts_node_type(obj);
                        if (strcmp(ok, "this") != 0 && strcmp(ok, "super") != 0) {
                            call.is_method = true;
                        }
                    }
                }
            }

            TSNode args = ts_node_child_by_field_name(node, TS_FIELD("arguments"));
            // ObjectScript stores args under oref_method/method_args, not the
            // generic "arguments" field.
            if (ts_node_is_null(args) && (ctx->language == CBM_LANG_OBJECTSCRIPT_UDL ||
                                          ctx->language == CBM_LANG_OBJECTSCRIPT_ROUTINE)) {
                TSNode oref = cbm_find_child_by_kind(node, "oref_method");
                if (!ts_node_is_null(oref)) {
                    args = cbm_find_child_by_kind(oref, "method_args");
                }
                if (ts_node_is_null(args)) {
                    args = cbm_find_child_by_kind(node, "method_args");
                }
            }
            if (!ts_node_is_null(args)) {
                extract_url_or_topic_args(ctx, args, &url_candidates);
                if (url_candidates.count > 0) {
                    call.first_string_arg = url_candidates.values[0];
                }
                /* #952: routes registered inside Laravel `prefix()->group()`
                 * closures must carry the composed path — the resolve passes
                 * only see the flat CBMCall, so the enclosing chain can only
                 * be read here where the AST still exists. */
                if (ctx->language == CBM_LANG_PHP && call.first_string_arg &&
                    call.first_string_arg[0] == '/' && call.callee_name &&
                    cbm_service_pattern_route_method(call.callee_name) != NULL) {
                    const char *gp = php_group_prefix_for_call(ctx->arena, node, ctx->source);
                    if (gp && gp[0]) {
                        const char *rel = call.first_string_arg;
                        while (*rel == '/') {
                            rel++;
                        }
                        call.first_string_arg =
                            rel[0] ? cbm_arena_sprintf(ctx->arena, "%s/%s", gp, rel)
                                   : cbm_arena_strndup(ctx->arena, gp, strlen(gp));
                    }
                }
                if (call.first_string_arg && call.first_string_arg[0] == '/') {
                    call.second_arg_name = extract_handler_arg(ctx, args);
                }
                if (ctx->language == CBM_LANG_OBJECTSCRIPT_UDL ||
                    ctx->language == CBM_LANG_OBJECTSCRIPT_ROUTINE) {
                    for (uint32_t ai = 0;
                         ai < ts_node_named_child_count(args) && call.arg_count < CBM_MAX_CALL_ARGS;
                         ai++) {
                        TSNode achild = ts_node_named_child(args, ai);
                        const char *ack = ts_node_type(achild);
                        if (strcmp(ack, "bracket") == 0) {
                            continue;
                        }
                        if (strcmp(ack, "method_arg") != 0) {
                            continue;
                        }
                        CBMCallArg *ca = &call.args[call.arg_count];
                        memset(ca, 0, sizeof(*ca));
                        ca->index = call.arg_count;
                        ca->expr = cbm_node_text(ctx->arena, achild, ctx->source);
                        if (ca->expr && ca->expr[0]) {
                            call.arg_count++;
                        }
                    }
                } else {
                    extract_call_args(ctx, args, &call);
                }
            }

            cbm_calls_push(&ctx->result->calls, ctx->arena, call);

            /* Preserve every statically possible JavaScript endpoint as a
             * separate
             * call record. Resolution then emits one method-specific
             * HTTP_CALLS edge
             * per candidate without changing CBMCall's common
             * single-target
             * representation for other languages. */
            cbm_svc_kind_t candidate_svc = cbm_service_pattern_match(call.callee_name);
            bool enumerable_http = candidate_svc == CBM_SVC_HTTP ||
                                   cbm_service_pattern_is_global_fetch(call.callee_name);
            for (int ci = 1; enumerable_http && ci < url_candidates.count; ci++) {
                CBMCall alternative = call;
                call_set_url_candidate(&alternative, call.first_string_arg,
                                       url_candidates.values[ci]);
                cbm_calls_push(&ctx->result->calls, ctx->arena, alternative);
            }

            /* Python callable wrappers do not contain a nested call node:
             *
             * `asyncio.to_thread(requests.post, url=...)` passes `requests.post`
             * as
             * a function reference. Materialize the equivalent service call
             * so HTTP
             * classification sees the callable and the forwarded URL. */
            if (ctx->language == CBM_LANG_PYTHON && call.first_string_arg && call.callee_name &&
                (strcmp(call.callee_name, "asyncio.to_thread") == 0 ||
                 strcmp(call.callee_name, "to_thread") == 0) &&
                !ts_node_is_null(args) && ts_node_named_child_count(args) > 0) {
                TSNode callable = ts_node_named_child(args, 0);
                const char *ck = ts_node_type(callable);
                if (strcmp(ck, "attribute") == 0 || strcmp(ck, "identifier") == 0) {
                    char *callable_name = cbm_node_text(ctx->arena, callable, ctx->source);
                    cbm_svc_kind_t wrapped_svc = cbm_service_pattern_match(callable_name);
                    if (wrapped_svc == CBM_SVC_HTTP || wrapped_svc == CBM_SVC_ASYNC) {
                        CBMCall wrapped = call;
                        wrapped.callee_name = callable_name;
                        cbm_calls_push(&ctx->result->calls, ctx->arena, wrapped);
                    }
                }
            }

            const char **dispatch_suffixes = cbm_string_dispatch_suffixes(ctx->language);
            if (dispatch_suffixes && !ts_node_is_null(args)) {
                const char *cn = call.callee_name;
                size_t len = cn ? strlen(cn) : 0;
                for (const char **nm = dispatch_suffixes; *nm; nm++) {
                    size_t nlen = strlen(*nm);
                    if (len >= nlen && strcmp(cn + len - nlen, *nm) == 0) {
                        const char *cls = extract_nth_string_arg(ctx, args, 0);
                        const char *mth = extract_nth_string_arg(ctx, args, 1);
                        if (cls && mth) {
                            CBMCall xcall = {0};
                            xcall.callee_name = cbm_arena_sprintf(ctx->arena, "%s.%s", cls, mth);
                            xcall.enclosing_func_qn = call.enclosing_func_qn;
                            cbm_calls_push(&ctx->result->calls, ctx->arena, xcall);
                        }
                        break;
                    }
                }
            }
        }
    }

    if (ctx->language == CBM_LANG_TSX || ctx->language == CBM_LANG_JAVASCRIPT) {
        extract_jsx_component_ref(ctx, node, ts_node_type(node), state->enclosing_func_qn);
    }

    if (ctx->language == CBM_LANG_JAVA) {
        extract_java_method_reference(ctx, node, ts_node_type(node), state->enclosing_func_qn);
    }

    if (ctx->language == CBM_LANG_KOTLIN) {
        extract_kotlin_operator_call(ctx, node, ts_node_type(node), state->enclosing_func_qn);
        extract_kotlin_desugared_calls(ctx, node, ts_node_type(node), state->enclosing_func_qn);
    }

    if (ctx->language == CBM_LANG_CPP || ctx->language == CBM_LANG_CUDA) {
        extract_cpp_operator_call(ctx, node, ts_node_type(node), state->enclosing_func_qn);
        extract_cpp_implicit_calls(ctx, node, ts_node_type(node), state->enclosing_func_qn);
    }
}
