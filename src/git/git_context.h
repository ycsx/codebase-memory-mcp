#ifndef CBM_GIT_CONTEXT_H
#define CBM_GIT_CONTEXT_H

#include <stdbool.h>

typedef enum {
    CBM_GIT_WORKTREE_UNKNOWN = 0,
    CBM_GIT_WORKTREE_CLEAN,
    CBM_GIT_WORKTREE_DIRTY,
} cbm_git_worktree_state_t;

typedef struct {
    bool is_git;
    bool is_worktree;
    bool is_detached;
    bool root_exists;
    cbm_git_worktree_state_t worktree_state;
    char *input_path;
    char *worktree_root;
    char *git_dir;
    char *git_common_dir;
    char *canonical_root;
    char *branch;
    char *branch_slug;
    char *head_sha;
    char *base_sha;
} cbm_git_context_t;

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out);
void cbm_git_context_free(cbm_git_context_t *ctx);
char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx);
int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size);

#endif
