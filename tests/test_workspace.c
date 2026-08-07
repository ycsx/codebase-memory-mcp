#include "test_framework.h"
#include "foundation/workspace.h"

#include <string.h>

static const char *WS_HOME = "/Users/dev";
static const char *WS_CACHE = "/Users/dev/.cache/codebase-memory-mcp";

TEST(ws_depth_counts_below_volume) {
    ASSERT_EQ(cbm_workspace_path_depth("/"), 0);
    ASSERT_EQ(cbm_workspace_path_depth("/etc"), 1);
    ASSERT_EQ(cbm_workspace_path_depth("/Users/dev"), 2);
    ASSERT_EQ(cbm_workspace_path_depth("/private/etc"), 1);
    ASSERT_EQ(cbm_workspace_path_depth("D:/repos"), 1);
    ASSERT_EQ(cbm_workspace_path_depth("D:\\repos\\app"), 2);
    ASSERT_EQ(cbm_workspace_path_depth("//srv/share"), 0);
    ASSERT_EQ(cbm_workspace_path_depth("//srv/share/proj"), 1);
    PASS();
}

TEST(ws_absolute_roots_are_denied) {
    ASSERT_EQ(cbm_workspace_classify_root("/", WS_HOME, WS_CACHE), CBM_WS_DENY_ABSOLUTE);
    ASSERT_EQ(cbm_workspace_classify_root("C:/", WS_HOME, WS_CACHE), CBM_WS_DENY_ABSOLUTE);
    ASSERT_EQ(cbm_workspace_classify_root("//srv/share", WS_HOME, WS_CACHE),
              CBM_WS_DENY_ABSOLUTE);
    ASSERT_EQ(cbm_workspace_classify_root("relative/path", WS_HOME, WS_CACHE),
              CBM_WS_DENY_ABSOLUTE);
    PASS();
}

TEST(ws_posix_top_level_is_too_shallow) {
    static const char *const paths[] = {"/etc", "/home", "/Users", "/var", "/usr",
                                        "/private/tmp"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        ASSERT_EQ(cbm_workspace_classify_root(paths[i], WS_HOME, WS_CACHE),
                  CBM_WS_DENY_TOO_SHALLOW);
    }
    PASS();
}

TEST(ws_normal_project_roots_are_allowed) {
    ASSERT_EQ(cbm_workspace_classify_root("/opt/sdk", WS_HOME, WS_CACHE), CBM_WS_ALLOW);
    ASSERT_EQ(cbm_workspace_classify_root("D:/repos", WS_HOME, WS_CACHE), CBM_WS_ALLOW);
    ASSERT_EQ(cbm_workspace_classify_root("//srv/share/proj", WS_HOME, WS_CACHE), CBM_WS_ALLOW);
    ASSERT_EQ(cbm_workspace_classify_root("/Users/dev/projects/app", WS_HOME, WS_CACHE),
              CBM_WS_ALLOW);
    ASSERT_EQ(cbm_workspace_classify_root("C:/Users/dev/projects/app", WS_HOME, WS_CACHE),
              CBM_WS_ALLOW);
    PASS();
}

TEST(ws_home_and_credentials_are_sensitive) {
    ASSERT_EQ(cbm_workspace_classify_root("/Users/dev", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("/Users/dev/.ssh/keys", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("/Users/dev/.aws", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("/Users/dev/.sshconfig", WS_HOME, WS_CACHE),
              CBM_WS_ALLOW);
    ASSERT_TRUE(cbm_workspace_verdict_is_overridable(CBM_WS_DENY_SENSITIVE));
    PASS();
}

TEST(ws_windows_system_trees_are_sensitive) {
    ASSERT_EQ(cbm_workspace_classify_root("C:/Windows/System32", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("C:/Users", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("C:/Program Files/app", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("C:/users/dev/.SSH", WS_HOME, WS_CACHE),
              CBM_WS_DENY_SENSITIVE);
    ASSERT_EQ(cbm_workspace_classify_root("C:/dev/Windows-app", WS_HOME, WS_CACHE),
              CBM_WS_ALLOW);
    PASS();
}

TEST(ws_every_verdict_has_a_reason) {
    ASSERT_TRUE(strlen(cbm_workspace_verdict_reason(CBM_WS_DENY_TOO_SHALLOW)) > 20);
    ASSERT_NOT_NULL(cbm_workspace_verdict_reason(CBM_WS_DENY_ABSOLUTE));
    ASSERT_NOT_NULL(cbm_workspace_verdict_reason(CBM_WS_DENY_SENSITIVE));
    PASS();
}

SUITE(workspace) {
    RUN_TEST(ws_depth_counts_below_volume);
    RUN_TEST(ws_absolute_roots_are_denied);
    RUN_TEST(ws_posix_top_level_is_too_shallow);
    RUN_TEST(ws_normal_project_roots_are_allowed);
    RUN_TEST(ws_home_and_credentials_are_sensitive);
    RUN_TEST(ws_windows_system_trees_are_sensitive);
    RUN_TEST(ws_every_verdict_has_a_reason);
}
