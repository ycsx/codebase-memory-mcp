/* Deterministic, repository-local Markdown references. */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/hash_table.h"
#include "foundation/limits.h"
#include "graph_buffer/graph_buffer.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cbm_pipeline_ctx_t *ctx;
    CBMHashTable *files;
    const cbm_gbuf_node_t *document;
    const cbm_gbuf_node_t **sections;
    int section_count;
    int added;
    bool failed;
    const char *read_status;
    bool html_comment;
    size_t inline_ticks;
    bool skipped_multiline_code;
} document_links_t;

static bool links_cancelled(const document_links_t *links) {
    return links->ctx->cancelled && atomic_load(links->ctx->cancelled);
}

static bool links_owned_edge(const cbm_gbuf_edge_t *edge, void *userdata) {
    (void)userdata;
    if (strcmp(edge->type, "REFERENCES") != 0 || !edge->properties_json) {
        return false;
    }
    yyjson_doc *json = yyjson_read(edge->properties_json, strlen(edge->properties_json), 0);
    if (!json) {
        return false;
    }
    const char *producer = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(json), "producer"));
    bool owned = producer && strcmp(producer, "document_links") == 0;
    yyjson_doc_free(json);
    return owned;
}

/* Lexical normalization never resolves a reference outside the indexed root. */
static char *links_normalize(const char *base, const char *text) {
    if (!text[0] || text[0] == '/' || strchr(text, ':') || strchr(text, '\\') ||
        strchr(text, '?')) {
        return NULL;
    }
    size_t n = strlen(base) + strlen(text) + 2;
    char *joined = malloc(n);
    char *out = malloc(n);
    if (!joined || !out) {
        free(joined);
        free(out);
        return NULL;
    }
    snprintf(joined, n, "%s%s%s", base, base[0] ? "/" : "", text);
    char *fragment = strchr(joined, '#');
    if (fragment) {
        *fragment = '\0';
    }
    size_t used = 0;
    for (char *p = joined; *p;) {
        while (*p == '/') {
            p++;
        }
        char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (!len || (len == 1 && start[0] == '.')) {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (!used) {
                free(joined);
                free(out);
                return NULL;
            }
            while (used && out[used - 1] != '/') {
                used--;
            }
            if (used) {
                used--;
            }
            continue;
        }
        if (used) {
            out[used++] = '/';
        }
        memcpy(out + used, start, len);
        used += len;
    }
    out[used] = '\0';
    free(joined);
    return out;
}

static const cbm_gbuf_node_t *links_source(const document_links_t *links, int line) {
    const cbm_gbuf_node_t *best = links->document;
    for (int i = 0; i < links->section_count; i++) {
        const cbm_gbuf_node_t *section = links->sections[i];
        if (section->start_line <= line && section->end_line >= line &&
            (best == links->document || section->start_line > best->start_line ||
             (section->start_line == best->start_line && section->end_line < best->end_line))) {
            best = section;
        }
    }
    return best;
}

static bool links_code_symbol(const cbm_gbuf_node_t *node) {
    static const char *labels[] = {"Function", "Method", "Class",    "Struct", "Interface", "Enum",
                                   "Type",     "Trait",  "Variable", "Field",  "Module"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (strcmp(node->label, labels[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void links_emit(document_links_t *links, const cbm_gbuf_node_t *target, const char *text,
                       const char *kind, int line) {
    const cbm_gbuf_node_t *source = links_source(links, line);
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    cbm_gbuf_find_edges_by_source_type(links->ctx->gbuf, source->id, "REFERENCES", &edges, &count);
    for (int i = 0; i < count; i++) {
        if (edges[i]->target_id == target->id) {
            return;
        }
    }
    yyjson_mut_doc *json = yyjson_mut_doc_new(NULL);
    if (!json) {
        links->failed = true;
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(json);
    yyjson_mut_doc_set_root(json, root);
    yyjson_mut_obj_add_str(json, root, "producer", "document_links");
    yyjson_mut_obj_add_str(json, root, "source", kind);
    yyjson_mut_obj_add_real(json, root, "confidence", 1.0);
    yyjson_mut_val *span = yyjson_mut_obj(json);
    yyjson_mut_obj_add_int(json, span, "start_line", line);
    yyjson_mut_obj_add_int(json, span, "end_line", line);
    yyjson_mut_obj_add_val(json, root, "document_span", span);
    yyjson_mut_obj_add_str(json, root, "matched_text", text);
    yyjson_mut_obj_add_str(json, root, "target_qualified_name", target->qualified_name);
    yyjson_mut_obj_add_int(json, root, "index_version", 1);
    char *props = yyjson_mut_write(json, 0, NULL);
    if (!props ||
        !cbm_gbuf_insert_edge(links->ctx->gbuf, source->id, target->id, "REFERENCES", props)) {
        links->failed = true;
    } else {
        links->added++;
    }
    free(props);
    yyjson_mut_doc_free(json);
}

static void links_resolve(document_links_t *links, const char *base, const char *start,
                          size_t length, bool explicit_link, int line) {
    if (!length || links->failed || links_cancelled(links)) {
        return;
    }
    char *text = malloc(length + 1);
    if (!text) {
        links->failed = true;
        return;
    }
    memcpy(text, start, length);
    text[length] = '\0';
    /* Whitespace, escapes and nested destination syntax need a full parser. */
    for (size_t i = 0; i < length; i++) {
        if (isspace((unsigned char)text[i]) || strchr("\\<>()", text[i])) {
            free(text);
            return;
        }
    }
    const cbm_gbuf_node_t *target = NULL;
    const char *kind = explicit_link ? "explicit_link" : "path_match";
    if (text[0] != '#') {
        char *relative = links_normalize(base, text);
        target = relative ? cbm_ht_get(links->files, relative) : NULL;
        free(relative);
        if (!explicit_link) {
            char *root_path = links_normalize("", text);
            const cbm_gbuf_node_t *root_target =
                root_path ? cbm_ht_get(links->files, root_path) : NULL;
            free(root_path);
            if (target && root_target && target->id != root_target->id) {
                free(text);
                return;
            }
            if (!target) {
                target = root_target;
            }
            const cbm_gbuf_node_t *symbol = cbm_gbuf_find_by_qn(links->ctx->gbuf, text);
            if (symbol && links_code_symbol(symbol)) {
                if (target && target->id != symbol->id) {
                    free(text);
                    return;
                }
                target = symbol;
                kind = "symbol_match";
            }
        }
    }
    if (target) {
        links_emit(links, target, text, kind, line);
    }
    free(text);
}

static void links_parse_line(document_links_t *links, const char *base, const char *line,
                             size_t length, int number) {
    for (size_t i = 0; i < length && !links->failed; i++) {
        if (links->html_comment) {
            if (length - i >= 3 && memcmp(line + i, "-->", 3) == 0) {
                links->html_comment = false;
                i += 2;
            }
            continue;
        }
        if (links->inline_ticks) {
            if (line[i] == '`') {
                size_t count = 1;
                while (i + count < length && line[i + count] == '`') {
                    count++;
                }
                if (count == links->inline_ticks) {
                    links->inline_ticks = 0;
                }
                i += count - 1;
            }
            continue;
        }
        if (line[i] == '\\') {
            i++;
            continue;
        }
        if (length - i >= 4 && memcmp(line + i, "<!--", 4) == 0) {
            links->html_comment = true;
            i += 3;
            continue;
        }
        if (line[i] == '`') {
            size_t ticks = 1;
            while (i + ticks < length && line[i + ticks] == '`') {
                ticks++;
            }
            size_t end = i + ticks;
            while (end < length) {
                if (line[end] != '`') {
                    end++;
                    continue;
                }
                size_t close = 1;
                while (end + close < length && line[end + close] == '`') {
                    close++;
                }
                if (close == ticks) {
                    break;
                }
                end += close;
            }
            if (end == length) {
                links->inline_ticks = ticks;
                links->skipped_multiline_code = true;
                return;
            }
            if (ticks == 1) {
                links_resolve(links, base, line + i + ticks, end - i - ticks, false, number);
            }
            i = end + ticks - 1;
            continue;
        }
        bool image = line[i] == '!' && i + 1 < length && line[i + 1] == '[';
        if (line[i] != '[' && !image) {
            continue;
        }
        size_t open = i + (image ? 1 : 0);
        size_t close = open + 1;
        while (close < length && line[close] != ']') {
            close++;
        }
        if (close + 1 >= length || line[close + 1] != '(') {
            continue;
        }
        size_t end = close + 2;
        while (end < length && line[end] != ')') {
            end++;
        }
        if (end == length) {
            return;
        }
        bool simple = true;
        for (size_t k = open + 1; k < close; k++) {
            if (line[k] == '[' || line[k] == '\\') {
                simple = false;
            }
        }
        if (!image && simple) {
            links_resolve(links, base, line + close + 2, end - close - 2, true, number);
        }
        i = end;
    }
}

static void links_read_document(document_links_t *links) {
    const char *relative = links->document->file_path;
    char *normalized = links_normalize("", relative);
    if (!normalized || strcmp(normalized, relative) != 0) {
        free(normalized);
        return;
    }
    free(normalized);
    size_t n = strlen(links->ctx->repo_path) + strlen(relative) + 2;
    char *path = malloc(n);
    char *base = strdup(relative);
    if (!path || !base) {
        free(path);
        free(base);
        links->failed = true;
        return;
    }
    snprintf(path, n, "%s/%s", links->ctx->repo_path, relative);
    char *slash = strrchr(base, '/');
    if (slash) {
        *slash = '\0';
    } else {
        base[0] = '\0';
    }
    FILE *file = cbm_fopen(path, "rb");
    free(path);
    if (!file) {
        free(base);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        free(base);
        return;
    }
    long size = ftell(file);
    if (size <= 0 || size > cbm_max_file_bytes() || fseek(file, 0, SEEK_SET) != 0) {
        links->read_status = size == 0                     ? NULL
                             : size > cbm_max_file_bytes() ? "oversized"
                                                           : "read_failed";
        fclose(file);
        free(base);
        return;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        free(base);
        links->failed = true;
        return;
    }
    size_t read = fread(text, 1, (size_t)size, file);
    bool read_ok = read == (size_t)size && !ferror(file);
    if (read_ok) {
        links->read_status = NULL;
    }
    fclose(file);
    text[read] = '\0';
    char fence = 0;
    size_t fence_width = 0;
    int number = 1;
    for (char *line = text; read_ok && *line && !links->failed && !links_cancelled(links);
         number++) {
        char *end = strchr(line, '\n');
        if (!end) {
            end = text + read;
        }
        size_t length = (size_t)(end - line);
        /* A continued inline construct takes precedence over apparent block markers. */
        if (!fence && (links->html_comment || links->inline_ticks)) {
            links_parse_line(links, base, line, length, number);
            line = *end ? end + 1 : end;
            continue;
        }
        size_t indent = 0;
        while (indent < length && line[indent] == ' ') {
            indent++;
        }
        /* Quoted blocks are outside this deliberately small Markdown subset. */
        if (indent < length && line[indent] == '>') {
            line = *end ? end + 1 : end;
            continue;
        }
        size_t marker_at = indent;
        if (!fence && indent <= 3 && marker_at + 1 < length) {
            size_t prefix = marker_at;
            if (line[prefix] == '-' || line[prefix] == '*' || line[prefix] == '+') {
                prefix++;
            } else {
                while (prefix < length && isdigit((unsigned char)line[prefix])) {
                    prefix++;
                }
                if (prefix > marker_at && prefix < length &&
                    (line[prefix] == '.' || line[prefix] == ')')) {
                    prefix++;
                } else {
                    prefix = marker_at;
                }
            }
            if (prefix > marker_at && prefix < length && line[prefix] == ' ') {
                while (prefix < length && line[prefix] == ' ') {
                    prefix++;
                }
                marker_at = prefix;
            }
        }
        size_t width = 0;
        char marker = marker_at < length ? line[marker_at] : 0;
        if ((fence || indent <= 3) && (marker == '`' || marker == '~')) {
            while (marker_at + width < length && line[marker_at + width] == marker) {
                width++;
            }
        }
        if (!fence && width >= 3) {
            fence = marker;
            fence_width = width;
        } else if (fence && marker == fence && width >= fence_width) {
            bool closing = true;
            for (size_t k = marker_at + width; k < length; k++) {
                if (!isspace((unsigned char)line[k])) {
                    closing = false;
                }
            }
            if (closing) {
                fence = 0;
            }
        } else if (!fence && indent < 4 && marker != '\t') {
            links_parse_line(links, base, line, length, number);
        }
        line = *end ? end + 1 : end;
    }
    free(text);
    free(base);
}

static void links_record_status(document_links_t *links) {
    const cbm_gbuf_node_t *node = links->document;
    const char *props = node->properties_json ? node->properties_json : "{}";
    yyjson_doc *old = yyjson_read(props, strlen(props), 0);
    yyjson_mut_doc *json = old ? yyjson_doc_mut_copy(old, NULL) : yyjson_mut_doc_new(NULL);
    if (!json) {
        yyjson_doc_free(old);
        links->failed = true;
        return;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(json);
    if (!root || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(json);
        yyjson_mut_doc_set_root(json, root);
    }
    yyjson_mut_obj_remove_key(root, "document_links");
    yyjson_mut_val *status = yyjson_mut_obj(json);
    const char *reason = links->failed                   ? "allocation_failed"
                         : links_cancelled(links)        ? "cancelled"
                         : links->read_status            ? links->read_status
                         : links->skipped_multiline_code ? "unsupported_multiline_code"
                                                         : NULL;
    yyjson_mut_obj_add_str(json, status, "status", reason ? "limited" : "ok");
    if (reason) {
        yyjson_mut_obj_add_str(json, status, "reason", reason);
    }
    yyjson_mut_obj_add_str(json, status, "scope", "single_line_links_and_exact_inline_code");
    yyjson_mut_obj_add_str(json, status, "excluded_syntax",
                           "html_comments,multiline_inline_code,quoted_blocks");
    yyjson_mut_obj_add_int(json, status, "index_version", 1);
    yyjson_mut_obj_add_val(json, root, "document_links", status);
    char *updated = yyjson_mut_write(json, 0, NULL);
    if (!updated ||
        !cbm_gbuf_upsert_node(links->ctx->gbuf, node->label, node->name, node->qualified_name,
                              node->file_path, node->start_line, node->end_line, updated)) {
        links->failed = true;
    }
    free(updated);
    yyjson_mut_doc_free(json);
    yyjson_doc_free(old);
}

int cbm_pipeline_pass_document_links(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf || !ctx->repo_path) {
        return -1;
    }
    document_links_t links = {.ctx = ctx};
    if (links_cancelled(&links)) {
        return -1;
    }
    links.files = cbm_ht_create(256);
    if (!links.files) {
        return -1;
    }
    const cbm_gbuf_node_t **files = NULL;
    const cbm_gbuf_node_t **documents = NULL;
    const cbm_gbuf_node_t **sections = NULL;
    int file_count = 0, document_count = 0, section_count = 0;
    cbm_gbuf_find_by_label(ctx->gbuf, "File", &files, &file_count);
    cbm_gbuf_find_by_label(ctx->gbuf, "Document", &documents, &document_count);
    cbm_gbuf_find_by_label(ctx->gbuf, "Section", &sections, &section_count);
    for (int i = 0; i < file_count; i++) {
        if (files[i]->file_path) {
            cbm_ht_set(links.files, files[i]->file_path, (void *)files[i]);
        }
    }
    if (section_count) {
        links.sections = malloc((size_t)section_count * sizeof(*links.sections));
        if (!links.sections) {
            cbm_ht_free(links.files);
            return -1;
        }
    }
    cbm_gbuf_delete_edges_if(ctx->gbuf, links_owned_edge, NULL);
    for (int i = 0; i < document_count && !links.failed && !links_cancelled(&links); i++) {
        links.document = documents[i];
        links.read_status = "read_failed";
        links.html_comment = false;
        links.inline_ticks = 0;
        links.skipped_multiline_code = false;
        if (!links.document->file_path) {
            continue;
        }
        links.section_count = 0;
        for (int j = 0; j < section_count; j++) {
            if (sections[j]->file_path &&
                strcmp(sections[j]->file_path, links.document->file_path) == 0) {
                links.sections[links.section_count++] = sections[j];
            }
        }
        links_read_document(&links);
        links_record_status(&links);
    }
    free(links.sections);
    cbm_ht_free(links.files);
    return links.failed || links_cancelled(&links) ? -1 : links.added;
}
