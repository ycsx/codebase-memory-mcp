#ifndef CBM_MCP_REMOTE_AUTH_H
#define CBM_MCP_REMOTE_AUTH_H

#include "mcp/mcp.h"

#include <stdbool.h>
#include <stddef.h>

#define CBM_REMOTE_KEY_ID_LEN 12
#define CBM_REMOTE_PRINCIPAL_MAX 128
#define CBM_REMOTE_KIND_MAX 16

typedef struct cbm_remote_auth_store cbm_remote_auth_store_t;

typedef struct {
    char key_id[CBM_REMOTE_KEY_ID_LEN + 1];
    char principal[CBM_REMOTE_PRINCIPAL_MAX];
    char kind[CBM_REMOTE_KIND_MAX];
    cbm_mcp_tool_profile_t tool_profile;
    bool source_read;
    bool index_write;
    bool delete_write;
    bool admin;
    char **projects;
    size_t project_count;
} cbm_remote_identity_t;

typedef struct {
    const char *principal;
    const char *kind;
    cbm_mcp_tool_profile_t tool_profile;
    bool source_read;
    bool index_write;
    bool delete_write;
    bool admin;
    const char *const *projects;
    size_t project_count;
} cbm_remote_key_options_t;

/* Resolve the default persistent key store in the cache directory. */
bool cbm_remote_auth_default_path(char *out, size_t outsz);

/* Open and validate a key store. When create_if_missing is true, an empty
 * versioned store is created atomically. */
cbm_remote_auth_store_t *cbm_remote_auth_store_open(const char *path, bool create_if_missing);
void cbm_remote_auth_store_close(cbm_remote_auth_store_t *store);

/* Reload changed files before comparing the presented high-entropy key hash. */
bool cbm_remote_auth_authenticate(cbm_remote_auth_store_t *store, const char *token,
                                  cbm_remote_identity_t *identity);

/* Key lifecycle. Plaintext is returned only by create/rotate and is never
 * persisted. Caller owns returned strings. */
int cbm_remote_auth_create_key(cbm_remote_auth_store_t *store,
                               const cbm_remote_key_options_t *options, char **plaintext_key,
                               char key_id[CBM_REMOTE_KEY_ID_LEN + 1]);
int cbm_remote_auth_revoke_key(cbm_remote_auth_store_t *store, const char *key_id);
int cbm_remote_auth_rotate_key(cbm_remote_auth_store_t *store, const char *key_id,
                               char **plaintext_key, char new_key_id[CBM_REMOTE_KEY_ID_LEN + 1]);

/* JSON contains principals and policy metadata, never key hashes or plaintext. */
char *cbm_remote_auth_list_json(cbm_remote_auth_store_t *store);

bool cbm_remote_identity_copy(cbm_remote_identity_t *dst, const cbm_remote_identity_t *src);
void cbm_remote_identity_free(cbm_remote_identity_t *identity);
bool cbm_remote_identity_project_allowed(const cbm_remote_identity_t *identity,
                                         const char *project);

/* OS-backed cryptographic randomness shared with the HTTP transport for
 * unguessable session/request nonces. */
bool cbm_remote_random_bytes(void *out, size_t len);

#endif /* CBM_MCP_REMOTE_AUTH_H */
