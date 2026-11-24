/* Wire format used to recognize Desktop-managed local client configurations. */
#ifndef CBM_DESKTOP_ENDPOINT_H
#define CBM_DESKTOP_ENDPOINT_H

#include <stdbool.h>
#include <string.h>

#define CBM_DESKTOP_URL_PREFIX "http://127.0.0.1:"
#define CBM_DESKTOP_TOKEN_PREFIX "cbm-desktop-"

static inline bool cbm_desktop_token_valid(const char *token) {
    size_t prefix = sizeof(CBM_DESKTOP_TOKEN_PREFIX) - 1;
    return token && strncmp(token, CBM_DESKTOP_TOKEN_PREFIX, prefix) == 0 &&
           strlen(token + prefix) == 64 && strspn(token + prefix, "0123456789abcdef") == 64;
}

static inline bool cbm_desktop_url_valid(const char *url) {
    size_t prefix = sizeof(CBM_DESKTOP_URL_PREFIX) - 1;
    if (!url || strncmp(url, CBM_DESKTOP_URL_PREFIX, prefix) != 0) {
        return false;
    }
    const char *p = url + prefix;
    unsigned port = 0;
    while (*p >= '0' && *p <= '9') {
        port = port * 10U + (unsigned)(*p++ - '0');
        if (port > 65535U) {
            return false;
        }
    }
    return port > 0 && strcmp(p, "/mcp") == 0;
}

static inline bool cbm_desktop_bearer_valid(const char *value) {
    return value && strncmp(value, "Bearer ", 7) == 0 && cbm_desktop_token_valid(value + 7);
}
#endif
