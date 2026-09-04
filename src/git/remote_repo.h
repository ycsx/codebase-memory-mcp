#ifndef CBM_REMOTE_REPO_H
#define CBM_REMOTE_REPO_H

#include <stdbool.h>
#include <stddef.h>

enum {
    CBM_REMOTE_URL_MAX = 1024,
    CBM_REMOTE_BRANCH_MAX = 256,
    CBM_REMOTE_PROJECT_MAX = 256,
};

typedef struct {
    char remote_url[CBM_REMOTE_URL_MAX];
    char branch[CBM_REMOTE_BRANCH_MAX];
    int poll_interval_sec;
} cbm_remote_repo_config_t;

typedef void (*cbm_remote_repo_visit_fn)(const char *project_name, const char *root_path,
                                         const cbm_remote_repo_config_t *config, void *user_data);

bool cbm_remote_repo_validate_url(const char *url);
/* Validate and copy HTTPS, SSH, SCP-style SSH, or local file repository URLs.
 * HTTPS remains HTTPS so public repositories do not unexpectedly require SSH. */
bool cbm_remote_repo_normalize_url(const char *url, char *out, size_t out_size);
bool cbm_remote_repo_validate_branch(const char *branch);
bool cbm_remote_repo_default_project(const char *url, char *out, size_t out_size);
bool cbm_remote_repo_managed_path(const char *project_name, char *out, size_t out_size);

/* Clone or reuse a managed repository and persist its polling metadata. */
int cbm_remote_repo_prepare(const char *project_name, const char *remote_url, const char *branch,
                            int poll_interval_sec, char *root_out, size_t root_out_size,
                            char *error_out, size_t error_out_size);

/* Load metadata from <root>/.git/cbm-remote.json. */
int cbm_remote_repo_load(const char *root_path, cbm_remote_repo_config_t *out);

/* Compare the configured remote branch and update the managed worktree.
 * Returns 1 when HEAD changed, 0 when unchanged, -1 on error. */
int cbm_remote_repo_sync(const char *root_path, const cbm_remote_repo_config_t *config,
                         char *remote_sha_out, size_t remote_sha_out_size, char *error_out,
                         size_t error_out_size);

/* Enumerate managed repositories under the current cache directory. */
int cbm_remote_repo_foreach(cbm_remote_repo_visit_fn visit, void *user_data);

/* Stop automatic polling without deleting the clone. */
int cbm_remote_repo_disable(const char *root_path);

/* Delete a clone only when root_path is this project's validated managed path.
 * Returns 1 when removed, 0 when the path is not a managed clone, -1 on cleanup failure. */
int cbm_remote_repo_remove_managed(const char *project_name, const char *root_path);

#endif /* CBM_REMOTE_REPO_H */
