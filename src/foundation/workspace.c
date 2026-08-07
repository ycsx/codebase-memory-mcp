#include "foundation/workspace.h"

#include "foundation/compat_fs.h"
#include "foundation/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WS_MIN_DEPTH_POSIX = 2,
    WS_MIN_DEPTH_WINDOWS = 1,
};

static bool ws_is_sep(char c) {
    return c == '/' || c == '\\';
}

static bool ws_is_windows_style(const char *path) {
    return path && path[0] && path[1] == ':' &&
           ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
}

static bool ws_is_unc(const char *path) {
    return path && ws_is_sep(path[0]) && ws_is_sep(path[1]);
}

static size_t ws_volume_prefix_len(const char *path) {
    if (!path || !path[0]) {
        return 0;
    }
    if (ws_is_unc(path)) {
        size_t i = 2;
        while (path[i] && !ws_is_sep(path[i])) {
            i++;
        }
        while (ws_is_sep(path[i])) {
            i++;
        }
        while (path[i] && !ws_is_sep(path[i])) {
            i++;
        }
        return i;
    }
    if (ws_is_sep(path[0])) {
        return 1;
    }
    if (ws_is_windows_style(path)) {
        return ws_is_sep(path[2]) ? 3 : 2;
    }
    return 0;
}

int cbm_workspace_path_depth(const char *canonical_path) {
    size_t prefix = ws_volume_prefix_len(canonical_path);
    if (prefix == 0) {
        return 0;
    }
    int depth = 0;
    const char *p = canonical_path + prefix;
    /* macOS canonicalizes /etc, /tmp and /var below /private. */
    if (strncmp(p, "private", 7) == 0 && (ws_is_sep(p[7]) || p[7] == '\0')) {
        p += 7;
    }
    while (*p) {
        while (ws_is_sep(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        depth++;
        while (*p && !ws_is_sep(*p)) {
            p++;
        }
    }
    return depth;
}

static char ws_fold(char c, bool fold_case) {
    if (fold_case && c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool ws_component_equals(const char *component, size_t len, const char *name,
                                bool fold_case) {
    if (strlen(name) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (ws_fold(component[i], fold_case) != ws_fold(name[i], fold_case)) {
            return false;
        }
    }
    return true;
}

static const char *const WS_CREDENTIAL_NAMES[] = {
    ".ssh",
    ".aws",
    ".gnupg",
    ".gpg",
    ".kube",
    ".docker",
    ".netrc",
    "_netrc",
    ".azure",
    ".gcloud",
    ".git-credentials",
    "Keychains",
    ".password-store",
    ".authinfo",
};

static const char *const WS_WINDOWS_SYSTEM_TREES[] = {
    "Windows",
    "ProgramData",
    "Program Files",
    "Program Files (x86)",
};

static const char *const WS_WINDOWS_USER_TREE = "Users";

static bool ws_first_component_matches(const char *path, const char *const *names, size_t count,
                                       bool fold_case) {
    const char *p = path + ws_volume_prefix_len(path);
    while (ws_is_sep(*p)) {
        p++;
    }
    const char *start = p;
    while (*p && !ws_is_sep(*p)) {
        p++;
    }
    size_t len = (size_t)(p - start);
    for (size_t i = 0; i < count; i++) {
        if (ws_component_equals(start, len, names[i], fold_case)) {
            return true;
        }
    }
    return false;
}

static bool ws_any_component_matches(const char *path, const char *const *names, size_t count,
                                     bool fold_case) {
    const char *p = path + ws_volume_prefix_len(path);
    while (*p) {
        while (ws_is_sep(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && !ws_is_sep(*p)) {
            p++;
        }
        size_t len = (size_t)(p - start);
        for (size_t i = 0; i < count; i++) {
            if (ws_component_equals(start, len, names[i], fold_case)) {
                return true;
            }
        }
    }
    return false;
}

static bool ws_paths_equal(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    bool fold_case = ws_is_windows_style(a) || ws_is_windows_style(b);
    while (*a && *b) {
        char ca = ws_is_sep(*a) ? '/' : ws_fold(*a, fold_case);
        char cb = ws_is_sep(*b) ? '/' : ws_fold(*b, fold_case);
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    while (ws_is_sep(*a)) {
        a++;
    }
    while (ws_is_sep(*b)) {
        b++;
    }
    return *a == '\0' && *b == '\0';
}

cbm_ws_verdict_t cbm_workspace_classify_root(const char *canonical_path, const char *home_dir,
                                             const char *cache_dir) {
    (void)cache_dir;
    if (!canonical_path || !canonical_path[0] || ws_volume_prefix_len(canonical_path) == 0) {
        return CBM_WS_DENY_ABSOLUTE;
    }

    bool windows_style = ws_is_windows_style(canonical_path);
    int depth = cbm_workspace_path_depth(canonical_path);
    if (depth == 0) {
        return CBM_WS_DENY_ABSOLUTE;
    }
    if (home_dir && home_dir[0] && ws_paths_equal(canonical_path, home_dir)) {
        return CBM_WS_DENY_SENSITIVE;
    }

    int min_depth =
        (windows_style || ws_is_unc(canonical_path)) ? WS_MIN_DEPTH_WINDOWS : WS_MIN_DEPTH_POSIX;
    if (depth < min_depth) {
        return CBM_WS_DENY_TOO_SHALLOW;
    }

    if (ws_any_component_matches(canonical_path, WS_CREDENTIAL_NAMES,
                                 sizeof(WS_CREDENTIAL_NAMES) / sizeof(WS_CREDENTIAL_NAMES[0]),
                                 windows_style)) {
        return CBM_WS_DENY_SENSITIVE;
    }
    if (windows_style && ws_first_component_matches(canonical_path, WS_WINDOWS_SYSTEM_TREES,
                                                    sizeof(WS_WINDOWS_SYSTEM_TREES) /
                                                        sizeof(WS_WINDOWS_SYSTEM_TREES[0]),
                                                    true)) {
        return CBM_WS_DENY_SENSITIVE;
    }
    if (windows_style && depth == 1 &&
        ws_first_component_matches(canonical_path, &WS_WINDOWS_USER_TREE, 1, true)) {
        return CBM_WS_DENY_SENSITIVE;
    }
    return CBM_WS_ALLOW;
}

const char *cbm_workspace_verdict_reason(cbm_ws_verdict_t verdict) {
    switch (verdict) {
    case CBM_WS_ALLOW:
        return "allowed";
    case CBM_WS_DENY_TOO_SHALLOW:
        return "path is too broad to index as one root; choose a project directory below it";
    case CBM_WS_DENY_ABSOLUTE:
        return "path is a filesystem, drive, or share root and cannot be indexed";
    case CBM_WS_DENY_SENSITIVE:
        return "path is a home, system, or credential directory";
    default:
        return "refused";
    }
}

bool cbm_workspace_verdict_is_overridable(cbm_ws_verdict_t verdict) {
    return verdict == CBM_WS_DENY_SENSITIVE;
}

bool cbm_path_within_root(const char *root_path, const char *abs_path) {
    if (!root_path || !abs_path) {
        return false;
    }
    char real_root[4096];
    char real_path[4096];
    if (!cbm_canonical_path(root_path, real_root, sizeof(real_root)) ||
        !cbm_canonical_path(abs_path, real_path, sizeof(real_path))) {
        return false;
    }
#ifdef _WIN32
    for (char *p = real_root; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    for (char *p = real_path; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
#endif
    size_t root_len = strlen(real_root);
    return strncmp(real_path, real_root, root_len) == 0 &&
           (real_path[root_len] == '/' || real_path[root_len] == '\0');
}

bool cbm_workspace_root_allowed(const char *canonical_path, const char *home_dir,
                                const char *cache_dir, const char *configured_root, char *err,
                                size_t err_sz) {
    if (err && err_sz > 0) {
        err[0] = '\0';
    }
    if (!canonical_path || !canonical_path[0]) {
        if (err && err_sz > 0) {
            snprintf(err, err_sz, "no repository path given");
        }
        return false;
    }
    if (configured_root && configured_root[0] &&
        !cbm_path_within_root(configured_root, canonical_path)) {
        if (err && err_sz > 0) {
            snprintf(err, err_sz, "%s is outside the allowed root", canonical_path);
        }
        return false;
    }
    cbm_ws_verdict_t verdict = cbm_workspace_classify_root(canonical_path, home_dir, cache_dir);
    if (verdict == CBM_WS_ALLOW) {
        return true;
    }
    if (err && err_sz > 0) {
        snprintf(err, err_sz, "%s: %s", canonical_path, cbm_workspace_verdict_reason(verdict));
    }
    return false;
}

const char *cbm_workspace_home_dir(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        return home;
    }
    home = getenv("USERPROFILE");
    return home && home[0] ? home : NULL;
}

const char *cbm_workspace_cache_dir(void) {
    return cbm_resolve_cache_dir();
}
