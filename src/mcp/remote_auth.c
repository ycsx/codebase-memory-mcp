#include "mcp/remote_auth.h"

#include "foundation/compat_fs.h"
#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <yyjson/yyjson.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

enum {
    AUTH_VERSION = 1,
    AUTH_HASH_HEX_LEN = CBM_SHA256_HEX_LEN,
    AUTH_KEY_BYTES = 32,
    AUTH_KEY_PREFIX_LEN = 4,
    AUTH_TIMESTAMP_LEN = 32,
    AUTH_LOCK_TIMEOUT_MS = 5000,
    AUTH_LOCK_STALE_SEC = 30,
    AUTH_LOCK_RETRY_MS = 25,
};

typedef struct {
    char id[CBM_REMOTE_KEY_ID_LEN + 1];
    char hash[AUTH_HASH_HEX_LEN + 1];
    char principal[CBM_REMOTE_PRINCIPAL_MAX];
    char kind[CBM_REMOTE_KIND_MAX];
    cbm_mcp_tool_profile_t tool_profile;
    bool source_read;
    bool index_write;
    bool delete_write;
    bool admin;
    bool revoked;
    char created_at[AUTH_TIMESTAMP_LEN];
    char revoked_at[AUTH_TIMESTAMP_LEN];
    char **projects;
    size_t project_count;
} cbm_remote_key_t;

struct cbm_remote_auth_store {
    char *path;
    cbm_remote_key_t *keys;
    size_t key_count;
    int64_t file_size;
    int64_t file_mtime_ns;
    cbm_mutex_t mutex;
};

typedef struct {
    char *path;
    FILE *file;
} auth_file_lock_t;

/* cbm_mutex_t protects callers in one process; the JSON store can also be
 * shared by the UI and MCP processes.  Serialize mutations with a short-lived
 * create-exclusive lock so two writers cannot overwrite each other's update. */
static bool auth_file_lock_acquire(const cbm_remote_auth_store_t *store, auth_file_lock_t *lock) {
    if (!store || !store->path || !lock) {
        return false;
    }
    memset(lock, 0, sizeof(*lock));
    size_t path_len = strlen(store->path);
    lock->path = malloc(path_len + 6U);
    if (!lock->path) {
        return false;
    }
    snprintf(lock->path, path_len + 6U, "%s.lock", store->path);

    const int attempts = AUTH_LOCK_TIMEOUT_MS / AUTH_LOCK_RETRY_MS;
    for (int attempt = 0; attempt < attempts; attempt++) {
        FILE *file = cbm_fopen(lock->path, "wbx");
        if (file) {
            (void)fprintf(file, "pid-lock\n");
            (void)fflush(file);
            lock->file = file;
            return true;
        }

        cbm_file_stat_t stat = {0};
        if (cbm_stat_utf8(lock->path, &stat) == 0) {
            int64_t now_ns = (int64_t)time(NULL) * 1000000000LL;
            if (now_ns > stat.mtime_ns &&
                now_ns - stat.mtime_ns >= (int64_t)AUTH_LOCK_STALE_SEC * 1000000000LL) {
                (void)cbm_unlink(lock->path);
                continue;
            }
        }
        struct timespec nap = {0, AUTH_LOCK_RETRY_MS * 1000000L};
        (void)cbm_nanosleep(&nap, NULL);
    }
    free(lock->path);
    memset(lock, 0, sizeof(*lock));
    return false;
}

static void auth_file_lock_release(auth_file_lock_t *lock) {
    if (!lock) {
        return;
    }
    if (lock->file) {
        (void)fclose(lock->file);
    }
    if (lock->path) {
        (void)cbm_unlink(lock->path);
    }
    free(lock->path);
    memset(lock, 0, sizeof(*lock));
}

static char *auth_strdup(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t len = strlen(value);
    char *copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, value, len + 1);
    }
    return copy;
}

static bool constant_time_equal(const char *left, const char *right) {
    if (!left || !right) {
        return false;
    }
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t max_len = left_len > right_len ? left_len : right_len;
    unsigned int diff = (unsigned int)(left_len ^ right_len);
    for (size_t i = 0; i < max_len; i++) {
        unsigned char a = i < left_len ? (unsigned char)left[i] : 0U;
        unsigned char b = i < right_len ? (unsigned char)right[i] : 0U;
        diff |= (unsigned int)(a ^ b);
    }
    return diff == 0U;
}

static void iso8601_now(char out[AUTH_TIMESTAMP_LEN]) {
    time_t now = time(NULL);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    strftime(out, AUTH_TIMESTAMP_LEN, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

bool cbm_remote_random_bytes(void *out, size_t len) {
    if (!out || len == 0) {
        return false;
    }
#ifdef _WIN32
    if (len > UINT32_MAX) {
        return false;
    }
    return SystemFunction036(out, (ULONG)len) != 0;
#else
    FILE *file = cbm_fopen("/dev/urandom", "rb");
    if (!file) {
        return false;
    }
    size_t got = fread(out, 1, len, file);
    fclose(file);
    return got == len;
#endif
}

static bool valid_identifier(const char *value, size_t max_len) {
    if (!value || !value[0] || strlen(value) >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20U || *p == 0x7fU) {
            return false;
        }
    }
    return true;
}

static bool valid_kind(const char *kind) {
    return kind &&
           (strcmp(kind, "user") == 0 || strcmp(kind, "ci") == 0 || strcmp(kind, "admin") == 0);
}

static const char *profile_name(cbm_mcp_tool_profile_t profile) {
    return profile == CBM_MCP_TOOL_PROFILE_SCOUT ? "scout" : "analysis";
}

static bool parse_profile(const char *name, cbm_mcp_tool_profile_t *profile) {
    if (!name || !profile) {
        return false;
    }
    if (strcmp(name, "analysis") == 0) {
        *profile = CBM_MCP_TOOL_PROFILE_ANALYSIS;
        return true;
    }
    if (strcmp(name, "scout") == 0) {
        *profile = CBM_MCP_TOOL_PROFILE_SCOUT;
        return true;
    }
    return false;
}

static bool valid_hash(const char *hash) {
    if (!hash || strlen(hash) != AUTH_HASH_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < AUTH_HASH_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)hash[i])) {
            return false;
        }
    }
    return true;
}

static void free_projects(char **projects, size_t count) {
    if (!projects) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(projects[i]);
    }
    free(projects);
}

static void key_free(cbm_remote_key_t *key) {
    if (!key) {
        return;
    }
    free_projects(key->projects, key->project_count);
    memset(key, 0, sizeof(*key));
}

static void keys_free(cbm_remote_key_t *keys, size_t count) {
    if (!keys) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        key_free(&keys[i]);
    }
    free(keys);
}

static bool parse_projects(yyjson_val *value, char ***projects_out, size_t *count_out) {
    *projects_out = NULL;
    *count_out = 0;
    if (!value || !yyjson_is_arr(value) || yyjson_arr_size(value) == 0) {
        return false;
    }
    size_t count = yyjson_arr_size(value);
    char **projects = calloc(count, sizeof(*projects));
    if (!projects) {
        return false;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(value, idx, max, item) {
        const char *name = yyjson_is_str(item) ? yyjson_get_str(item) : NULL;
        if (!valid_identifier(name, 256)) {
            free_projects(projects, idx);
            return false;
        }
        projects[idx] = auth_strdup(name);
        if (!projects[idx]) {
            free_projects(projects, idx);
            return false;
        }
    }
    *projects_out = projects;
    *count_out = count;
    return true;
}

static bool parse_key(yyjson_val *value, cbm_remote_key_t *key) {
    if (!value || !yyjson_is_obj(value) || !key) {
        return false;
    }
    const char *id = yyjson_get_str(yyjson_obj_get(value, "id"));
    const char *hash = yyjson_get_str(yyjson_obj_get(value, "hash"));
    const char *principal = yyjson_get_str(yyjson_obj_get(value, "principal"));
    const char *kind = yyjson_get_str(yyjson_obj_get(value, "kind"));
    const char *profile = yyjson_get_str(yyjson_obj_get(value, "profile"));
    const char *created_at = yyjson_get_str(yyjson_obj_get(value, "created_at"));
    const char *revoked_at = yyjson_get_str(yyjson_obj_get(value, "revoked_at"));
    if (!id || strlen(id) != CBM_REMOTE_KEY_ID_LEN || !valid_hash(hash) ||
        !valid_identifier(principal, CBM_REMOTE_PRINCIPAL_MAX) || !valid_kind(kind) ||
        !parse_profile(profile, &key->tool_profile) ||
        !parse_projects(yyjson_obj_get(value, "projects"), &key->projects, &key->project_count)) {
        return false;
    }
    snprintf(key->id, sizeof(key->id), "%s", id);
    snprintf(key->hash, sizeof(key->hash), "%s", hash);
    snprintf(key->principal, sizeof(key->principal), "%s", principal);
    snprintf(key->kind, sizeof(key->kind), "%s", kind);
    snprintf(key->created_at, sizeof(key->created_at), "%s", created_at ? created_at : "");
    snprintf(key->revoked_at, sizeof(key->revoked_at), "%s", revoked_at ? revoked_at : "");
    key->source_read = yyjson_is_true(yyjson_obj_get(value, "source_read"));
    key->index_write = yyjson_is_true(yyjson_obj_get(value, "index"));
    key->delete_write = yyjson_is_true(yyjson_obj_get(value, "delete"));
    key->admin = yyjson_is_true(yyjson_obj_get(value, "admin"));
    key->revoked = yyjson_is_true(yyjson_obj_get(value, "revoked"));
    return true;
}

static char *read_file(const char *path, size_t *size_out) {
    *size_out = 0;
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    *size_out = got;
    return data;
}

static int load_locked(cbm_remote_auth_store_t *store) {
    size_t size = 0;
    char *data = read_file(store->path, &size);
    if (!data) {
        return -1;
    }
    yyjson_doc *doc = yyjson_read(data, size, 0);
    free(data);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *version = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "version") : NULL;
    yyjson_val *keys = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "keys") : NULL;
    if (!yyjson_is_uint(version) || yyjson_get_uint(version) != AUTH_VERSION ||
        !yyjson_is_arr(keys)) {
        yyjson_doc_free(doc);
        return -1;
    }
    size_t count = yyjson_arr_size(keys);
    cbm_remote_key_t *parsed = count ? calloc(count, sizeof(*parsed)) : NULL;
    if (count && !parsed) {
        yyjson_doc_free(doc);
        return -1;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(keys, idx, max, item) {
        if (!parse_key(item, &parsed[idx])) {
            keys_free(parsed, count);
            yyjson_doc_free(doc);
            return -1;
        }
        for (size_t prior = 0; prior < idx; prior++) {
            if (strcmp(parsed[prior].id, parsed[idx].id) == 0) {
                keys_free(parsed, count);
                yyjson_doc_free(doc);
                return -1;
            }
        }
    }
    yyjson_doc_free(doc);
    keys_free(store->keys, store->key_count);
    store->keys = parsed;
    store->key_count = count;
    cbm_file_stat_t stat = {0};
    if (cbm_stat_utf8(store->path, &stat) == 0) {
        store->file_size = stat.size;
        store->file_mtime_ns = stat.mtime_ns;
    }
    return 0;
}

static yyjson_mut_val *key_to_json(yyjson_mut_doc *doc, const cbm_remote_key_t *key,
                                   bool include_hash) {
    if (!doc || !key) {
        return NULL;
    }
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    if (!item) {
        return NULL;
    }
    bool ok = yyjson_mut_obj_add_strcpy(doc, item, "id", key->id);
    if (include_hash) {
        ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "hash", key->hash);
    }
    ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "principal", key->principal);
    ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "kind", key->kind);
    ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "profile", profile_name(key->tool_profile));
    ok = ok && yyjson_mut_obj_add_bool(doc, item, "source_read", key->source_read);
    ok = ok && yyjson_mut_obj_add_bool(doc, item, "index", key->index_write);
    ok = ok && yyjson_mut_obj_add_bool(doc, item, "delete", key->delete_write);
    ok = ok && yyjson_mut_obj_add_bool(doc, item, "admin", key->admin);
    ok = ok && yyjson_mut_obj_add_bool(doc, item, "revoked", key->revoked);
    ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "created_at", key->created_at);
    if (key->revoked_at[0]) {
        ok = ok && yyjson_mut_obj_add_strcpy(doc, item, "revoked_at", key->revoked_at);
    }
    yyjson_mut_val *projects = yyjson_mut_arr(doc);
    if (!projects) {
        return NULL;
    }
    for (size_t i = 0; i < key->project_count; i++) {
        ok = ok && yyjson_mut_arr_add_strcpy(doc, projects, key->projects[i]);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, item, "projects", projects);
    return ok ? item : NULL;
}

static int sync_file(FILE *file) {
    if (fflush(file) != 0) {
        return -1;
    }
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

static int save_locked(cbm_remote_auth_store_t *store) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return -1;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    yyjson_mut_doc_set_root(doc, root);
    if (!yyjson_mut_obj_add_uint(doc, root, "version", AUTH_VERSION)) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    yyjson_mut_val *keys = yyjson_mut_arr(doc);
    if (!keys) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    for (size_t i = 0; i < store->key_count; i++) {
        yyjson_mut_val *item = key_to_json(doc, &store->keys[i], true);
        if (!item || !yyjson_mut_arr_add_val(keys, item)) {
            yyjson_mut_doc_free(doc);
            return -1;
        }
    }
    if (!yyjson_mut_obj_add_val(doc, root, "keys", keys)) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    size_t json_len = 0;
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &json_len);
    yyjson_mut_doc_free(doc);
    if (!json) {
        return -1;
    }
    char temp_path[4096];
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%lu", store->path, pid);
    FILE *file = cbm_fopen(temp_path, "wb");
    int rc = -1;
    if (file && fwrite(json, 1, json_len, file) == json_len && fputc('\n', file) != EOF &&
        sync_file(file) == 0 && fclose(file) == 0) {
        file = NULL;
#ifndef _WIN32
        (void)chmod(temp_path, S_IRUSR | S_IWUSR);
#endif
        if (cbm_rename_replace(temp_path, store->path) == 0) {
            rc = 0;
        }
    }
    if (file) {
        fclose(file);
    }
    if (rc != 0) {
        (void)cbm_unlink(temp_path);
    } else {
        cbm_file_stat_t stat = {0};
        if (cbm_stat_utf8(store->path, &stat) == 0) {
            store->file_size = stat.size;
            store->file_mtime_ns = stat.mtime_ns;
        }
    }
    free(json);
    return rc;
}

bool cbm_remote_auth_default_path(char *out, size_t outsz) {
    const char *cache = cbm_resolve_cache_dir();
    if (!out || outsz == 0 || !cache || !cache[0]) {
        return false;
    }
    /* The CLI can be used before the first index has created the cache. */
    if (!cbm_mkdir_p(cache, 0750)) {
        return false;
    }
    int written = snprintf(out, outsz, "%s/remote-auth.json", cache);
    return written > 0 && (size_t)written < outsz;
}

cbm_remote_auth_store_t *cbm_remote_auth_store_open(const char *path, bool create_if_missing) {
    if (!path || !path[0]) {
        return NULL;
    }
    cbm_remote_auth_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        return NULL;
    }
    store->path = auth_strdup(path);
    if (!store->path) {
        free(store->path);
        free(store);
        return NULL;
    }
    cbm_mutex_init(&store->mutex);
    if (cbm_file_exists(path)) {
        if (load_locked(store) != 0) {
            cbm_remote_auth_store_close(store);
            return NULL;
        }
    } else {
        if (!create_if_missing) {
            cbm_remote_auth_store_close(store);
            return NULL;
        }
        auth_file_lock_t file_lock = {0};
        bool ok = auth_file_lock_acquire(store, &file_lock);
        if (ok) {
            /* Another process may have created the file while we waited for
             * the lock; load it instead of replacing its keys. */
            ok = cbm_file_exists(path) ? load_locked(store) == 0 : save_locked(store) == 0;
            auth_file_lock_release(&file_lock);
        }
        if (!ok) {
            cbm_remote_auth_store_close(store);
            return NULL;
        }
    }
    return store;
}

void cbm_remote_auth_store_close(cbm_remote_auth_store_t *store) {
    if (!store) {
        return;
    }
    keys_free(store->keys, store->key_count);
    cbm_mutex_destroy(&store->mutex);
    free(store->path);
    free(store);
}

static bool reload_if_changed_locked(cbm_remote_auth_store_t *store) {
    cbm_file_stat_t stat = {0};
    if (cbm_stat_utf8(store->path, &stat) != 0) {
        return false;
    }
    if (stat.size != store->file_size || stat.mtime_ns != store->file_mtime_ns) {
        return load_locked(store) == 0;
    }
    return true;
}

bool cbm_remote_identity_copy(cbm_remote_identity_t *dst, const cbm_remote_identity_t *src) {
    if (!dst || !src) {
        return false;
    }
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->key_id, src->key_id, sizeof(dst->key_id));
    memcpy(dst->principal, src->principal, sizeof(dst->principal));
    memcpy(dst->kind, src->kind, sizeof(dst->kind));
    dst->tool_profile = src->tool_profile;
    dst->source_read = src->source_read;
    dst->index_write = src->index_write;
    dst->delete_write = src->delete_write;
    dst->admin = src->admin;
    if (src->project_count) {
        dst->projects = calloc(src->project_count, sizeof(*dst->projects));
        if (!dst->projects) {
            return false;
        }
        for (size_t i = 0; i < src->project_count; i++) {
            dst->projects[i] = auth_strdup(src->projects[i]);
            if (!dst->projects[i]) {
                cbm_remote_identity_free(dst);
                return false;
            }
        }
    }
    dst->project_count = src->project_count;
    return true;
}

void cbm_remote_identity_free(cbm_remote_identity_t *identity) {
    if (!identity) {
        return;
    }
    free_projects(identity->projects, identity->project_count);
    memset(identity, 0, sizeof(*identity));
}

static bool identity_from_key(cbm_remote_identity_t *identity, const cbm_remote_key_t *key) {
    cbm_remote_identity_t source = {0};
    snprintf(source.key_id, sizeof(source.key_id), "%s", key->id);
    snprintf(source.principal, sizeof(source.principal), "%s", key->principal);
    snprintf(source.kind, sizeof(source.kind), "%s", key->kind);
    source.tool_profile = key->tool_profile;
    source.source_read = key->source_read;
    source.index_write = key->index_write;
    source.delete_write = key->delete_write;
    source.admin = key->admin;
    source.projects = key->projects;
    source.project_count = key->project_count;
    return cbm_remote_identity_copy(identity, &source);
}

bool cbm_remote_auth_authenticate(cbm_remote_auth_store_t *store, const char *token,
                                  cbm_remote_identity_t *identity) {
    if (!store || !token || !token[0] || !identity) {
        return false;
    }
    char hash[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(token, strlen(token), hash);
    bool matched = false;
    cbm_mutex_lock(&store->mutex);
    if (reload_if_changed_locked(store)) {
        for (size_t i = 0; i < store->key_count; i++) {
            if (!store->keys[i].revoked && constant_time_equal(hash, store->keys[i].hash)) {
                matched = identity_from_key(identity, &store->keys[i]);
                break;
            }
        }
    }
    cbm_mutex_unlock(&store->mutex);
    memset(hash, 0, sizeof(hash));
    return matched;
}

bool cbm_remote_identity_project_allowed(const cbm_remote_identity_t *identity,
                                         const char *project) {
    if (!identity || !project || !project[0]) {
        return false;
    }
    for (size_t i = 0; i < identity->project_count; i++) {
        if (strcmp(identity->projects[i], "*") == 0 ||
            strcmp(identity->projects[i], project) == 0) {
            return true;
        }
    }
    return false;
}

static bool copy_options(cbm_remote_key_t *key, const cbm_remote_key_options_t *options) {
    if (!key || !options || !valid_identifier(options->principal, CBM_REMOTE_PRINCIPAL_MAX) ||
        !valid_kind(options->kind) || options->project_count == 0 || !options->projects ||
        (options->tool_profile != CBM_MCP_TOOL_PROFILE_ANALYSIS &&
         options->tool_profile != CBM_MCP_TOOL_PROFILE_SCOUT)) {
        return false;
    }
    snprintf(key->principal, sizeof(key->principal), "%s", options->principal);
    snprintf(key->kind, sizeof(key->kind), "%s", options->kind);
    key->tool_profile = options->tool_profile;
    key->source_read = options->source_read;
    key->index_write = options->index_write;
    key->delete_write = options->delete_write;
    key->admin = options->admin;
    key->projects = calloc(options->project_count, sizeof(*key->projects));
    if (!key->projects) {
        return false;
    }
    for (size_t i = 0; i < options->project_count; i++) {
        if (!valid_identifier(options->projects[i], 256)) {
            key_free(key);
            return false;
        }
        key->projects[i] = auth_strdup(options->projects[i]);
        if (!key->projects[i]) {
            key_free(key);
            return false;
        }
    }
    key->project_count = options->project_count;
    return true;
}

static int append_generated_key_locked(cbm_remote_auth_store_t *store,
                                       const cbm_remote_key_options_t *options,
                                       char **plaintext_key,
                                       char key_id[CBM_REMOTE_KEY_ID_LEN + 1]) {
    uint8_t random[AUTH_KEY_BYTES];
    if (!cbm_remote_random_bytes(random, sizeof(random))) {
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    char *plaintext = malloc(AUTH_KEY_PREFIX_LEN + sizeof(random) * 2 + 1);
    if (!plaintext) {
        memset(random, 0, sizeof(random));
        return -1;
    }
    memcpy(plaintext, "cbm_", AUTH_KEY_PREFIX_LEN);
    for (size_t i = 0; i < sizeof(random); i++) {
        plaintext[AUTH_KEY_PREFIX_LEN + i * 2] = hex[random[i] >> 4];
        plaintext[AUTH_KEY_PREFIX_LEN + i * 2 + 1] = hex[random[i] & 0x0f];
    }
    plaintext[AUTH_KEY_PREFIX_LEN + sizeof(random) * 2] = '\0';
    memset(random, 0, sizeof(random));

    cbm_remote_key_t key = {0};
    if (!copy_options(&key, options)) {
        memset(plaintext, 0, strlen(plaintext));
        free(plaintext);
        return -1;
    }
    cbm_sha256_hex(plaintext, strlen(plaintext), key.hash);
    memcpy(key.id, key.hash, CBM_REMOTE_KEY_ID_LEN);
    key.id[CBM_REMOTE_KEY_ID_LEN] = '\0';
    for (size_t i = 0; i < store->key_count; i++) {
        if (strcmp(store->keys[i].id, key.id) == 0) {
            key_free(&key);
            memset(plaintext, 0, strlen(plaintext));
            free(plaintext);
            return -1;
        }
    }
    iso8601_now(key.created_at);
    cbm_remote_key_t *grown = realloc(store->keys, (store->key_count + 1) * sizeof(*store->keys));
    if (!grown) {
        key_free(&key);
        memset(plaintext, 0, strlen(plaintext));
        free(plaintext);
        return -1;
    }
    store->keys = grown;
    store->keys[store->key_count++] = key;
    *plaintext_key = plaintext;
    snprintf(key_id, CBM_REMOTE_KEY_ID_LEN + 1, "%s", key.id);
    return 0;
}

int cbm_remote_auth_create_key(cbm_remote_auth_store_t *store,
                               const cbm_remote_key_options_t *options, char **plaintext_key,
                               char key_id[CBM_REMOTE_KEY_ID_LEN + 1]) {
    if (!store || !options || !plaintext_key || !key_id) {
        return -1;
    }
    *plaintext_key = NULL;
    key_id[0] = '\0';
    cbm_mutex_lock(&store->mutex);
    auth_file_lock_t file_lock = {0};
    if (!auth_file_lock_acquire(store, &file_lock) || load_locked(store) != 0) {
        auth_file_lock_release(&file_lock);
        cbm_mutex_unlock(&store->mutex);
        return -1;
    }
    size_t old_count = store->key_count;
    int rc = append_generated_key_locked(store, options, plaintext_key, key_id);
    if (rc == 0 && save_locked(store) != 0) {
        key_free(&store->keys[store->key_count - 1]);
        store->key_count = old_count;
        memset(*plaintext_key, 0, strlen(*plaintext_key));
        free(*plaintext_key);
        *plaintext_key = NULL;
        key_id[0] = '\0';
        rc = -1;
    }
    auth_file_lock_release(&file_lock);
    cbm_mutex_unlock(&store->mutex);
    return rc;
}

static cbm_remote_key_t *find_key(cbm_remote_auth_store_t *store, const char *key_id) {
    if (!key_id || strlen(key_id) != CBM_REMOTE_KEY_ID_LEN) {
        return NULL;
    }
    for (size_t i = 0; i < store->key_count; i++) {
        if (strcmp(store->keys[i].id, key_id) == 0) {
            return &store->keys[i];
        }
    }
    return NULL;
}

int cbm_remote_auth_revoke_key(cbm_remote_auth_store_t *store, const char *key_id) {
    if (!store) {
        return -1;
    }
    cbm_mutex_lock(&store->mutex);
    auth_file_lock_t file_lock = {0};
    if (!auth_file_lock_acquire(store, &file_lock) || load_locked(store) != 0) {
        auth_file_lock_release(&file_lock);
        cbm_mutex_unlock(&store->mutex);
        return -1;
    }
    cbm_remote_key_t *key = find_key(store, key_id);
    int rc = -1;
    if (key && !key->revoked) {
        key->revoked = true;
        iso8601_now(key->revoked_at);
        rc = save_locked(store);
        if (rc != 0) {
            key->revoked = false;
            key->revoked_at[0] = '\0';
        }
    }
    auth_file_lock_release(&file_lock);
    cbm_mutex_unlock(&store->mutex);
    return rc;
}

int cbm_remote_auth_rotate_key(cbm_remote_auth_store_t *store, const char *key_id,
                               char **plaintext_key, char new_key_id[CBM_REMOTE_KEY_ID_LEN + 1]) {
    if (!store || !plaintext_key || !new_key_id) {
        return -1;
    }
    *plaintext_key = NULL;
    new_key_id[0] = '\0';
    cbm_mutex_lock(&store->mutex);
    auth_file_lock_t file_lock = {0};
    if (!auth_file_lock_acquire(store, &file_lock) || load_locked(store) != 0) {
        auth_file_lock_release(&file_lock);
        cbm_mutex_unlock(&store->mutex);
        return -1;
    }
    cbm_remote_key_t *old = find_key(store, key_id);
    if (!old || old->revoked) {
        auth_file_lock_release(&file_lock);
        cbm_mutex_unlock(&store->mutex);
        return -1;
    }
    cbm_remote_key_options_t options = {
        .principal = old->principal,
        .kind = old->kind,
        .tool_profile = old->tool_profile,
        .source_read = old->source_read,
        .index_write = old->index_write,
        .delete_write = old->delete_write,
        .admin = old->admin,
        .projects = (const char *const *)old->projects,
        .project_count = old->project_count,
    };
    size_t old_count = store->key_count;
    if (append_generated_key_locked(store, &options, plaintext_key, new_key_id) != 0) {
        auth_file_lock_release(&file_lock);
        cbm_mutex_unlock(&store->mutex);
        return -1;
    }
    old = find_key(store, key_id);
    old->revoked = true;
    iso8601_now(old->revoked_at);
    int rc = save_locked(store);
    if (rc != 0) {
        old->revoked = false;
        old->revoked_at[0] = '\0';
        key_free(&store->keys[store->key_count - 1]);
        store->key_count = old_count;
        memset(*plaintext_key, 0, strlen(*plaintext_key));
        free(*plaintext_key);
        *plaintext_key = NULL;
        new_key_id[0] = '\0';
    }
    auth_file_lock_release(&file_lock);
    cbm_mutex_unlock(&store->mutex);
    return rc;
}

char *cbm_remote_auth_list_json(cbm_remote_auth_store_t *store) {
    if (!store) {
        return NULL;
    }
    cbm_mutex_lock(&store->mutex);
    if (!reload_if_changed_locked(store)) {
        cbm_mutex_unlock(&store->mutex);
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        cbm_mutex_unlock(&store->mutex);
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        cbm_mutex_unlock(&store->mutex);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    if (!yyjson_mut_obj_add_uint(doc, root, "version", AUTH_VERSION)) {
        yyjson_mut_doc_free(doc);
        cbm_mutex_unlock(&store->mutex);
        return NULL;
    }
    yyjson_mut_val *keys = yyjson_mut_arr(doc);
    if (!keys) {
        yyjson_mut_doc_free(doc);
        cbm_mutex_unlock(&store->mutex);
        return NULL;
    }
    for (size_t i = 0; i < store->key_count; i++) {
        yyjson_mut_val *item = key_to_json(doc, &store->keys[i], false);
        if (!item || !yyjson_mut_arr_add_val(keys, item)) {
            yyjson_mut_doc_free(doc);
            cbm_mutex_unlock(&store->mutex);
            return NULL;
        }
    }
    if (!yyjson_mut_obj_add_val(doc, root, "keys", keys)) {
        yyjson_mut_doc_free(doc);
        cbm_mutex_unlock(&store->mutex);
        return NULL;
    }
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    cbm_mutex_unlock(&store->mutex);
    return json;
}
