#include "git/remote_repo.h"

#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "foundation/win_utf8.h"
#else
#include <errno.h>
#include <sys/stat.h>
#endif

enum {
    REMOTE_POLL_DEFAULT_SEC = 300,
    REMOTE_POLL_MIN_SEC = 60,
    REMOTE_POLL_MAX_SEC = 3600,
    REMOTE_PATH_MAX = 4096,
    REMOTE_OUTPUT_MAX = 4096,
};

static const char *const g_git_ssh_config =
    "core.sshCommand=ssh -o BatchMode=yes -o ConnectTimeout=15 "
    "-o StrictHostKeyChecking=accept-new";

static void set_error(char *out, size_t out_size, const char *message) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", message ? message : "remote git operation failed");
    }
}

static void set_git_error(char *out, size_t out_size, const char *message) {
    const char *detail = message && message[0] ? message : "remote git operation failed";
    const char *hint = NULL;
    if (strstr(detail, "Host key verification failed")) {
        hint = "Use HTTPS for a public repository. For SSH, initialize known_hosts for the "
               "service account and verify the server fingerprint.";
    } else if (strstr(detail, "Permission denied (publickey)")) {
        hint = "Register this machine's SSH deploy public key with the repository. On a managed "
               "server, run: sudo cbm-server git-key";
    } else if (strstr(detail, "could not read Username") ||
               strstr(detail, "Authentication failed")) {
        hint = "Private HTTPS repositories need non-interactive credentials; use an SSH URL and "
               "register the service deploy key instead.";
    }
    if (hint && out && out_size > 0) {
        snprintf(out, out_size, "%s\nHint: %s", detail, hint);
        return;
    }
    set_error(out, out_size, detail);
}

static int clamp_poll_interval(int seconds) {
    if (seconds <= 0) {
        return REMOTE_POLL_DEFAULT_SEC;
    }
    if (seconds < REMOTE_POLL_MIN_SEC) {
        return REMOTE_POLL_MIN_SEC;
    }
    if (seconds > REMOTE_POLL_MAX_SEC) {
        return REMOTE_POLL_MAX_SEC;
    }
    return seconds;
}

static bool validate_https_url(const char *url) {
    static const char prefix[] = "https://";
    const char *host = url + sizeof(prefix) - 1;
    const char *path = strchr(host, '/');
    if (!path || path == host || path[1] == '\0' || memchr(host, '@', (size_t)(path - host)) ||
        memchr(host, ':', (size_t)(path - host)) || strpbrk(path, "?#")) {
        return false;
    }
    return true;
}

bool cbm_remote_repo_validate_url(const char *url) {
    if (!url || !url[0] || strlen(url) >= CBM_REMOTE_URL_MAX ||
        !cbm_validate_shell_arg(url)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (isspace(*p) || iscntrl(*p)) {
            return false;
        }
    }
    if (strncmp(url, "https://", 8) == 0) {
        return validate_https_url(url);
    }
    if (strncmp(url, "ssh://", 6) == 0 || strncmp(url, "file://", 7) == 0) {
        return true;
    }
    /* SCP-style SSH URL: git@host:owner/repository.git */
    const char *at = strchr(url, '@');
    const char *colon = at ? strchr(at + 1, ':') : NULL;
    return at && at != url && colon && colon[1] != '\0';
}

bool cbm_remote_repo_normalize_url(const char *url, char *out, size_t out_size) {
    if (!url || !out || out_size == 0) {
        return false;
    }
    if (!cbm_remote_repo_validate_url(url)) {
        return false;
    }
    int n = snprintf(out, out_size, "%s", url);
    return n > 0 && (size_t)n < out_size;
}

bool cbm_remote_repo_validate_branch(const char *branch) {
    if (!branch || !branch[0] || strlen(branch) >= CBM_REMOTE_BRANCH_MAX ||
        !cbm_validate_shell_arg(branch) || branch[0] == '-' || branch[0] == '/' ||
        strstr(branch, "..") || strstr(branch, "@{") || strstr(branch, "//")) {
        return false;
    }
    size_t len = strlen(branch);
    if (branch[len - 1] == '/' || branch[len - 1] == '.' || strstr(branch, ".lock")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)branch; *p; p++) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '/')) {
            return false;
        }
    }
    return true;
}

bool cbm_remote_repo_default_project(const char *url, char *out, size_t out_size) {
    if (!cbm_remote_repo_validate_url(url) || !out || out_size < 2) {
        return false;
    }
    const char *end = url + strlen(url);
    while (end > url && end[-1] == '/') {
        end--;
    }
    const char *start = end;
    while (start > url && start[-1] != '/' && start[-1] != ':') {
        start--;
    }
    size_t len = (size_t)(end - start);
    if (len > 4 && strncmp(end - 4, ".git", 4) == 0) {
        len -= 4;
    }
    if (len == 0 || len >= out_size) {
        return false;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 1 < out_size; i++) {
        unsigned char c = (unsigned char)start[i];
        out[pos++] = (isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '-';
    }
    out[pos] = '\0';
    return cbm_validate_project_name(out);
}

static bool repos_dir(char *out, size_t out_size) {
    const char *cache = cbm_resolve_cache_dir();
    if (!cache || !out || out_size == 0) {
        return false;
    }
    int n = snprintf(out, out_size, "%s/repos", cache);
    return n > 0 && (size_t)n < out_size;
}

bool cbm_remote_repo_managed_path(const char *project_name, char *out, size_t out_size) {
    char base[REMOTE_PATH_MAX];
    if (!cbm_validate_project_name(project_name) || !repos_dir(base, sizeof(base))) {
        return false;
    }
    int n = snprintf(out, out_size, "%s/%s", base, project_name);
    return n > 0 && (size_t)n < out_size;
}

static bool marker_path(const char *root_path, char *out, size_t out_size) {
    int n = snprintf(out, out_size, "%s/.git/cbm-remote.json", root_path);
    return n > 0 && (size_t)n < out_size;
}

static int write_marker(const char *root_path, const char *remote_url, const char *branch,
                        int poll_interval_sec) {
    char path[REMOTE_PATH_MAX];
    char temp[REMOTE_PATH_MAX];
    if (!marker_path(root_path, path, sizeof(path))) {
        return -1;
    }
    int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(temp)) {
        return -1;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return -1;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", 1);
    yyjson_mut_obj_add_strcpy(doc, root, "remote_url", remote_url);
    yyjson_mut_obj_add_strcpy(doc, root, "branch", branch);
    yyjson_mut_obj_add_int(doc, root, "poll_interval_sec", clamp_poll_interval(poll_interval_sec));
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) {
        return -1;
    }

    FILE *file = cbm_fopen(temp, "wb");
    if (!file) {
        free(json);
        return -1;
    }
    bool ok = fputs(json, file) >= 0 && fputc('\n', file) != EOF && fclose(file) == 0;
    free(json);
    if (!ok) {
        (void)cbm_unlink(temp);
        return -1;
    }
    if (cbm_rename_replace(temp, path) != 0) {
        (void)cbm_unlink(temp);
        return -1;
    }
    return 0;
}

int cbm_remote_repo_load(const char *root_path, cbm_remote_repo_config_t *out) {
    if (!root_path || !out) {
        return -1;
    }
    char path[REMOTE_PATH_MAX];
    if (!marker_path(root_path, path, sizeof(path))) {
        return -1;
    }
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    if (size <= 0 || size > REMOTE_OUTPUT_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return -1;
    }
    size_t got = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[got] = '\0';
    yyjson_doc *doc = yyjson_read(data, got, 0);
    free(data);
    if (!doc) {
        return -1;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *url = yyjson_obj_get(root, "remote_url");
    yyjson_val *branch = yyjson_obj_get(root, "branch");
    yyjson_val *poll = yyjson_obj_get(root, "poll_interval_sec");
    const char *url_text = yyjson_is_str(url) ? yyjson_get_str(url) : NULL;
    const char *branch_text = yyjson_is_str(branch) ? yyjson_get_str(branch) : NULL;
    if (!cbm_remote_repo_validate_url(url_text) || !cbm_remote_repo_validate_branch(branch_text)) {
        yyjson_doc_free(doc);
        return -1;
    }
    snprintf(out->remote_url, sizeof(out->remote_url), "%s", url_text);
    snprintf(out->branch, sizeof(out->branch), "%s", branch_text);
    out->poll_interval_sec =
        clamp_poll_interval(yyjson_is_int(poll) ? (int)yyjson_get_int(poll) : 0);
    yyjson_doc_free(doc);
    return 0;
}

static int run_git(const char *const *argv, char *output, size_t output_size) {
    return cbm_exec_capture(argv, output, output_size);
}

static int remove_tree_no_follow(const char *path);

static bool parse_sha_for_ref(const char *output, const char *ref, char *sha, size_t sha_size) {
    const char *line = output;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *tab = memchr(line, '\t', len);
        if (tab && (size_t)(tab - line) < sha_size) {
            size_t sha_len = (size_t)(tab - line);
            bool hex = sha_len == 40 || sha_len == 64;
            for (size_t i = 0; hex && i < sha_len; i++) {
                hex = isxdigit((unsigned char)line[i]) != 0;
            }
            size_t ref_len = len - sha_len - 1;
            if (hex && strlen(ref) == ref_len && memcmp(tab + 1, ref, ref_len) == 0) {
                memcpy(sha, line, sha_len);
                sha[sha_len] = '\0';
                return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

int cbm_remote_repo_prepare(const char *project_name, const char *remote_url, const char *branch,
                            int poll_interval_sec, char *root_out, size_t root_out_size,
                            char *error_out, size_t error_out_size) {
    if (!cbm_validate_project_name(project_name) || !cbm_remote_repo_validate_url(remote_url) ||
        !cbm_remote_repo_validate_branch(branch) ||
        !cbm_remote_repo_managed_path(project_name, root_out, root_out_size)) {
        set_error(error_out, error_out_size, "invalid remote repository configuration");
        return -1;
    }

    char base[REMOTE_PATH_MAX];
    if (!repos_dir(base, sizeof(base)) || !cbm_mkdir_p(base, 0750)) {
        set_error(error_out, error_out_size, "cannot create managed repository directory");
        return -1;
    }

    if (cbm_is_dir(root_out)) {
        cbm_remote_repo_config_t existing;
        if (cbm_remote_repo_load(root_out, &existing) != 0 ||
            strcmp(existing.remote_url, remote_url) != 0 || strcmp(existing.branch, branch) != 0) {
            set_error(error_out, error_out_size,
                      "managed repository path already exists with different settings");
            return -1;
        }
        if (write_marker(root_out, remote_url, branch, poll_interval_sec) != 0) {
            set_error(error_out, error_out_size, "cannot update remote repository metadata");
            return -1;
        }
        return 0;
    }

    char output[REMOTE_OUTPUT_MAX] = {0};
    const char *const argv[] = {"git", "-c", g_git_ssh_config, "clone", "--no-tags",
                                "--single-branch", "--branch", branch, "--", remote_url,
                                root_out, NULL};
    int rc = run_git(argv, output, sizeof(output));
    if (rc != 0 || !cbm_is_dir(root_out)) {
        if (cbm_is_dir(root_out)) {
            (void)remove_tree_no_follow(root_out);
        }
        set_git_error(error_out, error_out_size, output[0] ? output : "git clone failed");
        return -1;
    }
    if (write_marker(root_out, remote_url, branch, poll_interval_sec) != 0) {
        set_error(error_out, error_out_size, "clone succeeded but metadata write failed");
        return -1;
    }
    return 0;
}

int cbm_remote_repo_sync(const char *root_path, const cbm_remote_repo_config_t *config,
                         char *remote_sha_out, size_t remote_sha_out_size, char *error_out,
                         size_t error_out_size) {
    if (!root_path || !config || !cbm_remote_repo_validate_url(config->remote_url) ||
        !cbm_remote_repo_validate_branch(config->branch)) {
        set_error(error_out, error_out_size, "invalid managed repository metadata");
        return -1;
    }

    char ref[CBM_REMOTE_BRANCH_MAX + 16];
    char remote_ref[CBM_REMOTE_BRANCH_MAX + 40];
    snprintf(ref, sizeof(ref), "refs/heads/%s", config->branch);
    snprintf(remote_ref, sizeof(remote_ref), "+%s:refs/remotes/origin/%s", ref, config->branch);

    char output[REMOTE_OUTPUT_MAX] = {0};
    const char *const ls_argv[] = {"git", "-c", g_git_ssh_config, "ls-remote", "--heads",
                                   "--exit-code", config->remote_url, ref, NULL};
    int rc = run_git(ls_argv, output, sizeof(output));
    char remote_sha[65] = {0};
    if (rc != 0 || !parse_sha_for_ref(output, ref, remote_sha, sizeof(remote_sha))) {
        set_git_error(error_out, error_out_size,
                      output[0] ? output : "remote branch was not found or is not reachable");
        return -1;
    }
    if (remote_sha_out && remote_sha_out_size > 0) {
        snprintf(remote_sha_out, remote_sha_out_size, "%s", remote_sha);
    }

    output[0] = '\0';
    const char *const head_argv[] = {"git", "-C", root_path, "rev-parse", "HEAD", NULL};
    rc = run_git(head_argv, output, sizeof(output));
    if (rc == 0 && strncmp(output, remote_sha, strlen(remote_sha)) == 0) {
        return 0;
    }

    output[0] = '\0';
    const char *const fetch_argv[] = {"git", "-c", g_git_ssh_config, "-C", root_path, "fetch",
                                      "--no-tags", "origin", remote_ref, NULL};
    rc = run_git(fetch_argv, output, sizeof(output));
    if (rc != 0) {
        set_git_error(error_out, error_out_size, output[0] ? output : "git fetch failed");
        return -1;
    }

    output[0] = '\0';
    const char *const reset_argv[] = {"git", "-C", root_path, "reset", "--hard", remote_sha,
                                      NULL};
    rc = run_git(reset_argv, output, sizeof(output));
    if (rc != 0) {
        set_error(error_out, error_out_size, output[0] ? output : "git reset failed");
        return -1;
    }
    return 1;
}

int cbm_remote_repo_foreach(cbm_remote_repo_visit_fn visit, void *user_data) {
    if (!visit) {
        return 0;
    }
    char base[REMOTE_PATH_MAX];
    if (!repos_dir(base, sizeof(base))) {
        return 0;
    }
    cbm_dir_t *dir = cbm_opendir(base);
    if (!dir) {
        return 0;
    }
    int count = 0;
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(dir)) != NULL) {
        if (!entry->is_dir || !cbm_validate_project_name(entry->name)) {
            continue;
        }
        char root[REMOTE_PATH_MAX];
        int n = snprintf(root, sizeof(root), "%s/%s", base, entry->name);
        if (n <= 0 || (size_t)n >= sizeof(root)) {
            continue;
        }
        cbm_remote_repo_config_t config;
        if (cbm_remote_repo_load(root, &config) == 0) {
            visit(entry->name, root, &config, user_data);
            count++;
        }
    }
    cbm_closedir(dir);
    return count;
}

int cbm_remote_repo_disable(const char *root_path) {
    char path[REMOTE_PATH_MAX];
    if (!root_path || !marker_path(root_path, path, sizeof(path))) {
        return -1;
    }
    return cbm_unlink(path);
}

static bool plain_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wide_path = cbm_utf8_to_wide(path);
    if (!wide_path) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(wide_path);
    free(wide_path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    struct stat state;
    return lstat(path, &state) == 0 && S_ISDIR(state.st_mode) && !S_ISLNK(state.st_mode);
#endif
}

static int remove_tree_no_follow(const char *path) {
    cbm_dir_t *directory = cbm_opendir(path);
    if (!directory) {
        return -1;
    }

    int result = 0;
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(directory)) != NULL) {
        char child[REMOTE_PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->name);
        if (n <= 0 || (size_t)n >= sizeof(child)) {
            result = -1;
            continue;
        }

        bool is_directory = entry->is_dir;
        bool is_symlink = entry->is_symlink;
#ifndef _WIN32
        if (entry->d_type == 0) {
            struct stat state;
            if (lstat(child, &state) != 0) {
                if (errno != ENOENT) {
                    result = -1;
                }
                continue;
            }
            is_directory = S_ISDIR(state.st_mode);
            is_symlink = S_ISLNK(state.st_mode);
        }
#endif

        int child_result;
        if (is_directory && !is_symlink) {
            child_result = remove_tree_no_follow(child);
        } else if (is_directory) {
#ifdef _WIN32
            child_result = cbm_rmdir(child); /* Remove the junction, never its target. */
#else
            child_result = cbm_unlink(child);
#endif
        } else {
            child_result = cbm_unlink(child);
        }
        if (child_result != 0) {
            result = -1;
        }
    }
    cbm_closedir(directory);
    if (result != 0) {
        return -1;
    }
    return cbm_rmdir(path);
}

int cbm_remote_repo_remove_managed(const char *project_name, const char *root_path) {
    char expected[REMOTE_PATH_MAX];
    char expected_canonical[REMOTE_PATH_MAX];
    char root_canonical[REMOTE_PATH_MAX];
    if (!project_name || !root_path || !plain_directory(root_path) ||
        !cbm_remote_repo_managed_path(project_name, expected, sizeof(expected)) ||
        !cbm_canonical_path(expected, expected_canonical, sizeof(expected_canonical)) ||
        !cbm_canonical_path(root_path, root_canonical, sizeof(root_canonical))) {
        return 0;
    }
#ifdef _WIN32
    if (_stricmp(expected_canonical, root_canonical) != 0) {
#else
    if (strcmp(expected_canonical, root_canonical) != 0) {
#endif
        return 0;
    }

    cbm_remote_repo_config_t config;
    if (cbm_remote_repo_load(root_path, &config) != 0) {
        return 0;
    }
    if (remove_tree_no_follow(root_path) == 0) {
        return 1;
    }

    /* Preserve enough metadata to retry or repair a partially removed clone. */
    if (cbm_is_dir(root_path)) {
        char git_dir[REMOTE_PATH_MAX];
        int n = snprintf(git_dir, sizeof(git_dir), "%s/.git", root_path);
        if (n > 0 && (size_t)n < sizeof(git_dir) && cbm_mkdir_p(git_dir, 0700)) {
            (void)write_marker(root_path, config.remote_url, config.branch,
                               config.poll_interval_sec);
        }
    }
    return -1;
}
