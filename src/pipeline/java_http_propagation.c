#include "pipeline_internal.h"

#include "foundation/log.h"
#include "service_patterns.h"
#include "tree_sitter/api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { JHP_MAX_WRAPPERS = 4096, JHP_MAX_PASSES = 8, JHP_MAX_TAINT_NAMES = 64, JHP_MAX_IDENT = 128 };

typedef struct {
    const CBMDefinition *def;
    int url_param;
    int method_param;
    const char *fixed_method;
} java_http_wrapper_t;

typedef struct {
    char names[JHP_MAX_TAINT_NAMES][JHP_MAX_IDENT];
    int count;
} java_taint_set_t;

static const char *leaf_name(const char *name) {
    if (!name) {
        return NULL;
    }
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

static bool contains_ci(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) {
        return false;
    }
    size_t n = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static bool name_looks_like_url(const char *name) {
    return contains_ci(name, "url") || contains_ci(name, "uri") || contains_ci(name, "endpoint");
}

static bool name_looks_like_method(const char *name) {
    return contains_ci(name, "method") || (name && strcasecmp(name, "verb") == 0);
}

static const char *http_method_literal(const char *value) {
    if (!value) {
        return NULL;
    }
    const char *leaf = leaf_name(value);
    static const char *const methods[] = {"GET",   "POST", "PUT",     "DELETE",
                                          "PATCH", "HEAD", "OPTIONS", NULL};
    for (int i = 0; methods[i]; i++) {
        if (strcasecmp(leaf, methods[i]) == 0) {
            return methods[i];
        }
    }
    return NULL;
}

static bool qn_is_http(const char *qn) {
    return cbm_service_pattern_match(qn) == CBM_SVC_HTTP;
}

static bool ends_with_ci(const char *text, const char *suffix) {
    if (!text || !suffix) {
        return false;
    }
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len && strcasecmp(text + text_len - suffix_len, suffix) == 0;
}

static bool call_receiver_matches_http_import(const CBMFileResult *result, const CBMCall *call) {
    if (!call->callee_name) {
        return false;
    }
    const char *dot = strrchr(call->callee_name, '.');
    if (!dot) {
        return false;
    }
    const char *receiver = call->callee_name;
    for (const char *p = dot; p > call->callee_name; p--) {
        if (p[-1] == '.') {
            receiver = p;
            break;
        }
    }
    size_t receiver_len = (size_t)(dot - receiver);
    if (receiver_len == 0 || receiver_len >= JHP_MAX_IDENT) {
        return false;
    }
    char receiver_name[JHP_MAX_IDENT];
    memcpy(receiver_name, receiver, receiver_len);
    receiver_name[receiver_len] = '\0';
    for (int i = 0; i < result->imports.count; i++) {
        const CBMImport *imp = &result->imports.items[i];
        const char *identity = imp->local_name ? imp->local_name : leaf_name(imp->module_path);
        if ((!qn_is_http(imp->module_path) && !qn_is_http(identity)) || !identity) {
            continue;
        }
        if (strcasecmp(receiver_name, identity) == 0 || ends_with_ci(receiver_name, identity)) {
            return true;
        }
    }
    return false;
}

static bool body_has_http_client(const char *body_tokens) {
    if (qn_is_http(body_tokens)) {
        return true;
    }
    static const char *const java_clients[] = {
        "resttemplate", "httpurlconnection", "urlconnection", "httpclient",      "webclient",
        "okhttp",       "unirest",           "retrofit",      "asynchttpclient", NULL};
    for (int i = 0; java_clients[i]; i++) {
        if (contains_ci(body_tokens, java_clients[i])) {
            return true;
        }
    }
    return false;
}

static const char *resolved_qn_for_call(const CBMFileResult *result, const CBMCall *call) {
    const char *leaf = leaf_name(call->callee_name);
    if (!leaf || !call->enclosing_func_qn) {
        return NULL;
    }
    const char *found = NULL;
    for (int i = 0; i < result->resolved_calls.count; i++) {
        const CBMResolvedCall *resolved = &result->resolved_calls.items[i];
        if (!resolved->caller_qn || !resolved->callee_qn ||
            strcmp(resolved->caller_qn, call->enclosing_func_qn) != 0 ||
            strcmp(leaf_name(resolved->callee_qn), leaf) != 0) {
            continue;
        }
        if (found && strcmp(found, resolved->callee_qn) != 0) {
            return NULL;
        }
        found = resolved->callee_qn;
    }
    return found;
}

static bool call_is_http_sink(const CBMFileResult *result, const CBMCall *call) {
    if (call->is_http_wrapper || qn_is_http(call->callee_name) ||
        call_receiver_matches_http_import(result, call)) {
        return true;
    }
    return qn_is_http(resolved_qn_for_call(result, call));
}

static const CBMCallArg *call_arg_at(const CBMCall *call, int positional_index) {
    for (int i = 0; i < call->arg_count; i++) {
        if (call->args[i].index == positional_index) {
            return &call->args[i];
        }
    }
    return NULL;
}

static CBMCallArg *mutable_call_arg_at(CBMCall *call, int positional_index) {
    return (CBMCallArg *)call_arg_at(call, positional_index);
}

static bool taint_has(const java_taint_set_t *set, const char *name, size_t len) {
    if (!set || !name || len == 0) {
        return false;
    }
    for (int i = 0; i < set->count; i++) {
        if (strlen(set->names[i]) == len && strncmp(set->names[i], name, len) == 0) {
            return true;
        }
    }
    return false;
}

static bool taint_add(java_taint_set_t *set, const char *name, size_t len) {
    if (!set || !name || len == 0 || len >= JHP_MAX_IDENT || set->count >= JHP_MAX_TAINT_NAMES ||
        taint_has(set, name, len)) {
        return false;
    }
    memcpy(set->names[set->count], name, len);
    set->names[set->count][len] = '\0';
    set->count++;
    return true;
}

static bool node_text_ident(const char *source, TSNode node, const char **start, size_t *len) {
    if (!source || ts_node_is_null(node) || strcmp(ts_node_type(node), "identifier") != 0) {
        return false;
    }
    uint32_t begin = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end <= begin || end - begin >= JHP_MAX_IDENT) {
        return false;
    }
    *start = source + begin;
    *len = (size_t)(end - begin);
    return true;
}

static bool node_contains_taint(TSNode root, const char *source, const java_taint_set_t *set) {
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        const char *text = NULL;
        size_t len = 0;
        if (node_text_ident(source, node, &text, &len) && taint_has(set, text, len)) {
            ts_tree_cursor_delete(&cursor);
            return true;
        }
        if (ts_tree_cursor_goto_first_child(&cursor)) {
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
    return false;
}

static TSNode find_def_ast_node(const CBMFileResult *result, const CBMDefinition *def) {
    if (!result->cached_tree || !def || def->start_line == 0) {
        return (TSNode){0};
    }
    TSNode root = ts_tree_root_node(result->cached_tree);
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        const char *kind = ts_node_type(node);
        uint32_t line = ts_node_start_point(node).row + 1;
        if (line == def->start_line && (strcmp(kind, "method_declaration") == 0 ||
                                        strcmp(kind, "constructor_declaration") == 0)) {
            ts_tree_cursor_delete(&cursor);
            return node;
        }
        if (line <= def->end_line && ts_tree_cursor_goto_first_child(&cursor)) {
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
    return (TSNode){0};
}

static void expand_taint_once(TSNode def_node, const char *source, java_taint_set_t *set,
                              bool *changed) {
    TSTreeCursor cursor = ts_tree_cursor_new(def_node);
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        const char *kind = ts_node_type(node);
        TSNode left = {0};
        TSNode right = {0};
        if (strcmp(kind, "variable_declarator") == 0) {
            left = ts_node_child_by_field_name(node, "name", 4);
            right = ts_node_child_by_field_name(node, "value", 5);
        } else if (strcmp(kind, "assignment_expression") == 0) {
            left = ts_node_child_by_field_name(node, "left", 4);
            right = ts_node_child_by_field_name(node, "right", 5);
        }
        const char *lhs = NULL;
        size_t lhs_len = 0;
        if (!ts_node_is_null(right) && node_text_ident(source, left, &lhs, &lhs_len) &&
            node_contains_taint(right, source, set) && taint_add(set, lhs, lhs_len)) {
            *changed = true;
        }
        if (ts_tree_cursor_goto_first_child(&cursor)) {
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
}

static void build_param_taint(const CBMFileResult *result, const CBMDefinition *def,
                              const char *source, int param_index, java_taint_set_t *set) {
    memset(set, 0, sizeof(*set));
    if (!def->param_names || !def->param_names[param_index]) {
        return;
    }
    taint_add(set, def->param_names[param_index], strlen(def->param_names[param_index]));
    TSNode def_node = find_def_ast_node(result, def);
    if (ts_node_is_null(def_node) || !source) {
        return;
    }
    for (int pass = 0; pass < JHP_MAX_PASSES; pass++) {
        bool changed = false;
        expand_taint_once(def_node, source, set, &changed);
        if (!changed) {
            break;
        }
    }
}

static bool expr_is_tainted(const char *expr, const java_taint_set_t *set) {
    if (!expr) {
        return false;
    }
    while (isspace((unsigned char)*expr) || *expr == '(') {
        expr++;
    }
    const char *start = expr;
    while (isalnum((unsigned char)*expr) || *expr == '_') {
        expr++;
    }
    const char *end = expr;
    while (isspace((unsigned char)*expr) || *expr == ')') {
        expr++;
    }
    return *expr == '\0' && taint_has(set, start, (size_t)(end - start));
}

static char *read_source_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *source = malloc((size_t)len + 1);
    if (!source) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(source, 1, (size_t)len, f);
    fclose(f);
    source[got] = '\0';
    return source;
}

static int find_flowing_param(const CBMFileResult *result, const CBMDefinition *def,
                              const char *source, const CBMCall *sink, bool method_param) {
    if (!def->param_names) {
        return -1;
    }
    for (int pi = 0; def->param_names[pi]; pi++) {
        if (!method_param && !name_looks_like_url(def->param_names[pi])) {
            continue;
        }
        if (method_param && !name_looks_like_method(def->param_names[pi])) {
            continue;
        }
        java_taint_set_t taint;
        build_param_taint(result, def, source, pi, &taint);
        for (int ai = 0; ai < sink->arg_count; ai++) {
            if (expr_is_tainted(sink->args[ai].expr, &taint)) {
                return pi;
            }
        }
    }
    return -1;
}

static const char *fixed_method_in_def(const CBMFileResult *result, const CBMDefinition *def) {
    for (int i = 0; i < result->calls.count; i++) {
        const CBMCall *call = &result->calls.items[i];
        if (!call->enclosing_func_qn || strcmp(call->enclosing_func_qn, def->qualified_name) != 0) {
            continue;
        }
        const char *leaf = leaf_name(call->callee_name);
        if (leaf && strcmp(leaf, "setRequestMethod") == 0) {
            const char *method = http_method_literal(call->first_string_arg);
            if (method) {
                return method;
            }
        }
    }
    return NULL;
}

static bool wrapper_already_present(const java_http_wrapper_t *wrappers, int count,
                                    const CBMDefinition *def) {
    for (int i = 0; i < count; i++) {
        if (wrappers[i].def == def) {
            return true;
        }
    }
    return false;
}

static int discover_wrappers(const cbm_file_info_t *files, int file_count, CBMFileResult **results,
                             java_http_wrapper_t *wrappers, int count) {
    for (int fi = 0; fi < file_count && count < JHP_MAX_WRAPPERS; fi++) {
        CBMFileResult *result = results[fi];
        if (!result || files[fi].language != CBM_LANG_JAVA) {
            continue;
        }
        char *owned_source = NULL;
        const char *source = result->source;
        if (!source) {
            owned_source = read_source_file(files[fi].path);
            source = owned_source;
        }
        for (int di = 0; di < result->defs.count && count < JHP_MAX_WRAPPERS; di++) {
            const CBMDefinition *def = &result->defs.items[di];
            if (!def->qualified_name || !def->param_names || !def->label ||
                strcmp(def->label, "Method") != 0 ||
                wrapper_already_present(wrappers, count, def)) {
                continue;
            }
            bool has_http_evidence = body_has_http_client(def->body_tokens);
            int url_param = -1;
            int method_param = -1;
            const char *fixed_method = NULL;
            for (int ci = 0; ci < result->calls.count; ci++) {
                const CBMCall *call = &result->calls.items[ci];
                if (!call->enclosing_func_qn ||
                    strcmp(call->enclosing_func_qn, def->qualified_name) != 0 ||
                    !call_is_http_sink(result, call)) {
                    continue;
                }
                has_http_evidence = true;
                if (url_param < 0) {
                    url_param = find_flowing_param(result, def, source, call, false);
                }
                if (method_param < 0) {
                    method_param = find_flowing_param(result, def, source, call, true);
                }
                if (!fixed_method && call->http_method) {
                    fixed_method = http_method_literal(call->http_method);
                }
            }
            if (!has_http_evidence) {
                continue;
            }
            if (url_param < 0) {
                for (int pi = 0; def->param_names[pi]; pi++) {
                    if (name_looks_like_url(def->param_names[pi])) {
                        url_param = pi;
                        break;
                    }
                }
            }
            if (url_param < 0) {
                continue;
            }
            if (!fixed_method) {
                fixed_method = fixed_method_in_def(result, def);
            }
            wrappers[count++] = (java_http_wrapper_t){.def = def,
                                                      .url_param = url_param,
                                                      .method_param = method_param,
                                                      .fixed_method = fixed_method};
        }
        free(owned_source);
    }
    return count;
}

static bool owner_matches(const char *owner, const char *owner_expr) {
    if (!owner || !owner_expr) {
        return false;
    }
    const char *simple = leaf_name(owner);
    size_t expr_len = strlen(owner_expr);
    size_t simple_len = strlen(simple);
    return strcmp(owner, owner_expr) == 0 || strcmp(simple, owner_expr) == 0 ||
           (expr_len > simple_len && strcmp(owner_expr + expr_len - simple_len, simple) == 0 &&
            owner_expr[expr_len - simple_len - 1] == '.');
}

static const char *resolve_constant_expr(const CBMFileResult *local, CBMFileResult *const *results,
                                         int file_count, const char *expr) {
    if (!expr || !expr[0]) {
        return NULL;
    }
    while (isspace((unsigned char)*expr) || *expr == '(') {
        expr++;
    }
    size_t len = strlen(expr);
    while (len > 0 && (isspace((unsigned char)expr[len - 1]) || expr[len - 1] == ')')) {
        len--;
    }
    if (len == 0 || len >= 512) {
        return NULL;
    }
    char clean[512];
    memcpy(clean, expr, len);
    clean[len] = '\0';
    char *dot = strrchr(clean, '.');
    const char *constant_name = dot ? dot + 1 : clean;
    if (!constant_name[0]) {
        return NULL;
    }
    if (!dot && local) {
        for (int i = local->string_constant_count - 1; i >= 0; i--) {
            if (strcmp(local->string_constants[i].name, constant_name) == 0) {
                return local->string_constants[i].value;
            }
        }
    }
    const char *owner_expr = NULL;
    if (dot) {
        *dot = '\0';
        owner_expr = clean;
    }
    const char *found = NULL;
    for (int fi = 0; fi < file_count; fi++) {
        const CBMFileResult *result = results[fi];
        if (!result) {
            continue;
        }
        for (int ci = 0; ci < result->string_constant_count; ci++) {
            const CBMStringConstant *constant = &result->string_constants[ci];
            if (strcmp(constant->name, constant_name) != 0 ||
                (owner_expr && !owner_matches(constant->owner, owner_expr))) {
                continue;
            }
            const char *value = constant->value;
            if (found && strcmp(found, value) != 0) {
                return NULL;
            }
            found = value;
        }
    }
    return found;
}

static bool wrapper_owner_matches_receiver(const java_http_wrapper_t *wrapper,
                                           const char *callee_name) {
    if (!wrapper->def->parent_class || !callee_name) {
        return false;
    }
    const char *dot = strrchr(callee_name, '.');
    if (!dot) {
        return false;
    }
    const char *receiver_start = callee_name;
    const char *receiver_dot = dot;
    for (const char *p = dot; p > callee_name; p--) {
        if (p[-1] == '.') {
            receiver_start = p;
            break;
        }
    }
    size_t receiver_len = (size_t)(receiver_dot - receiver_start);
    const char *owner = leaf_name(wrapper->def->parent_class);
    return strlen(owner) == receiver_len && strncasecmp(owner, receiver_start, receiver_len) == 0;
}

static const java_http_wrapper_t *match_wrapper(const java_http_wrapper_t *wrappers, int count,
                                                const CBMFileResult *result, const CBMCall *call) {
    const char *resolved = resolved_qn_for_call(result, call);
    if (resolved) {
        for (int i = 0; i < count; i++) {
            if (strcmp(resolved, wrappers[i].def->qualified_name) == 0) {
                return &wrappers[i];
            }
        }
    }
    const char *leaf = leaf_name(call->callee_name);
    const java_http_wrapper_t *found = NULL;
    int leaf_matches = 0;
    const java_http_wrapper_t *receiver_match = NULL;
    int receiver_matches = 0;
    for (int i = 0; i < count; i++) {
        if (!leaf || strcmp(leaf_name(wrappers[i].def->qualified_name), leaf) != 0) {
            continue;
        }
        found = &wrappers[i];
        leaf_matches++;
        if (wrapper_owner_matches_receiver(&wrappers[i], call->callee_name)) {
            receiver_match = &wrappers[i];
            receiver_matches++;
        }
    }
    return receiver_matches == 1 ? receiver_match : (leaf_matches == 1 ? found : NULL);
}

static bool propagate_calls(const cbm_file_info_t *files, int file_count, CBMFileResult **results,
                            const java_http_wrapper_t *wrappers, int wrapper_count, int *hinted) {
    bool changed = false;
    for (int fi = 0; fi < file_count; fi++) {
        CBMFileResult *result = results[fi];
        if (!result || files[fi].language != CBM_LANG_JAVA) {
            continue;
        }
        for (int ci = 0; ci < result->calls.count; ci++) {
            CBMCall *call = &result->calls.items[ci];
            const java_http_wrapper_t *wrapper =
                match_wrapper(wrappers, wrapper_count, result, call);
            if (!wrapper) {
                continue;
            }
            CBMCallArg *url_arg = mutable_call_arg_at(call, wrapper->url_param);
            if (!url_arg) {
                continue;
            }
            const char *url = url_arg->value;
            if (!url) {
                url = resolve_constant_expr(result, results, file_count, url_arg->expr);
            }
            char normalized[1024];
            if (!url ||
                !cbm_service_pattern_normalize_http_url(url, normalized, sizeof(normalized))) {
                continue;
            }
            if (!call->is_http_wrapper || call->first_string_arg != url || url_arg->value != url) {
                if (!call->is_http_wrapper) {
                    (*hinted)++;
                }
                call->is_http_wrapper = true;
                call->first_string_arg = url;
                url_arg->value = url;
                changed = true;
            }
            if (!call->http_method && wrapper->method_param >= 0) {
                const CBMCallArg *method_arg = call_arg_at(call, wrapper->method_param);
                const char *method = method_arg
                                         ? http_method_literal(method_arg->value ? method_arg->value
                                                                                 : method_arg->expr)
                                         : NULL;
                if (method) {
                    call->http_method = method;
                    changed = true;
                }
            }
            if (!call->http_method && wrapper->fixed_method) {
                call->http_method = wrapper->fixed_method;
                changed = true;
            }
        }
    }
    return changed;
}

int cbm_pipeline_propagate_java_http(const cbm_file_info_t *files, int file_count,
                                     CBMFileResult **result_cache) {
    if (!files || file_count <= 0 || !result_cache) {
        return 0;
    }
    java_http_wrapper_t *wrappers = calloc(JHP_MAX_WRAPPERS, sizeof(*wrappers));
    if (!wrappers) {
        return 0;
    }
    int wrapper_count = 0;
    int propagated_passes = 0;
    int hinted = 0;
    int constants = 0;
    for (int i = 0; i < file_count; i++) {
        if (result_cache[i] && files[i].language == CBM_LANG_JAVA) {
            constants += result_cache[i]->string_constant_count;
        }
    }
    for (int pass = 0; pass < JHP_MAX_PASSES; pass++) {
        int old_count = wrapper_count;
        wrapper_count = discover_wrappers(files, file_count, result_cache, wrappers, wrapper_count);
        bool changed =
            propagate_calls(files, file_count, result_cache, wrappers, wrapper_count, &hinted);
        if (changed) {
            propagated_passes++;
        }
        if (!changed && wrapper_count == old_count) {
            break;
        }
    }
    if (wrapper_count > 0 || constants > 0) {
        char wrapper_buf[32];
        char call_buf[32];
        char constant_buf[32];
        snprintf(wrapper_buf, sizeof(wrapper_buf), "%d", wrapper_count);
        snprintf(call_buf, sizeof(call_buf), "%d", hinted);
        snprintf(constant_buf, sizeof(constant_buf), "%d", constants);
        cbm_log_info("java_http.propagation", "wrappers", wrapper_buf, "calls", call_buf,
                     "constants", constant_buf);
    }
    free(wrappers);
    return propagated_passes;
}
