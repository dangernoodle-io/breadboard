#include "bb_dispatch_api.h"
#include "bb_log.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "dispatch_api";

/* ---------------------------------------------------------------------------
 * Internal route entry
 * ---------------------------------------------------------------------------*/
typedef struct {
    bb_http_method_t  method;
    const char       *path;
    bb_http_handler_fn handler;
} bb_dispatch_api_entry_t;

/* ---------------------------------------------------------------------------
 * File-scope state — no heap, no ESP, s_ prefix per house rules
 * ---------------------------------------------------------------------------*/
static bb_dispatch_api_entry_t s_dispatch[BB_DISPATCH_API_CAP];
static size_t                  s_count;
static bool                    s_warned;

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

void bb_dispatch_api_reset(void)
{
    memset(s_dispatch, 0, sizeof(s_dispatch));
    s_count  = 0;
    s_warned = false;
}

bb_err_t bb_dispatch_api_add(bb_http_method_t method, const char *path,
                              bb_http_handler_fn handler)
{
    if (s_count >= BB_DISPATCH_API_CAP) {
        return BB_ERR_NO_SPACE;
    }

    /* Duplicate-route safeguard: scan for an existing (method, path) pair.
     * Uses the same case-sensitive exact-match semantics as the dispatcher. */
    if (path != NULL) {
        size_t path_len = strlen(path);
        for (size_t i = 0; i < s_count; i++) {
            if (s_dispatch[i].method != method) continue;
            if (s_dispatch[i].path == NULL) continue;
            if (strlen(s_dispatch[i].path) != path_len) continue;
            if (memcmp(s_dispatch[i].path, path, path_len) == 0) {
                bb_log_w(TAG, "duplicate route %d %s ignored (first registration wins)",
                         (int)method, path);
#if defined(CONFIG_BB_HTTP_ROUTE_DUP_STRICT) && CONFIG_BB_HTTP_ROUTE_DUP_STRICT
                bb_log_e(TAG, "duplicate route %d %s — aborting (BB_HTTP_ROUTE_DUP_STRICT)",
                         (int)method, path);
                assert(0 && "duplicate (method,path) route registration — increase route uniqueness or disable BB_HTTP_ROUTE_DUP_STRICT");
#endif
                return BB_ERR_INVALID_STATE;
            }
        }
    }

    s_dispatch[s_count].method  = method;
    s_dispatch[s_count].path    = path;
    s_dispatch[s_count].handler = handler;
    s_count++;

    /* High-watermark warn: fire once when count crosses CAP-8. */
    if (!s_warned && s_count >= (size_t)(BB_DISPATCH_API_CAP - 8)) {
        s_warned = true;
        bb_log_w(TAG, "api dispatch table at %u/%u; %d slots remain",
                 (unsigned)s_count, (unsigned)BB_DISPATCH_API_CAP,
                 (int)(BB_DISPATCH_API_CAP - (int)s_count));
    }

    return BB_OK;
}

bb_dispatch_api_result_t bb_dispatch_api_lookup(bb_http_method_t method,
                                                const char *uri,
                                                bb_http_handler_fn *out_handler)
{
    if (uri == NULL || out_handler == NULL) {
        return BB_DISPATCH_API_MISS;
    }

    /* Compute path length: up to first '?' (strip query string). */
    size_t path_len = 0;
    while (uri[path_len] != '\0' && uri[path_len] != '?') {
        path_len++;
    }

    bool path_matched = false;

    for (size_t i = 0; i < s_count; i++) {
        const char *entry_path = s_dispatch[i].path;
        if (entry_path == NULL) {
            continue;
        }
        size_t entry_len = strlen(entry_path);

        /* Exact match: same length and same bytes, entry NUL-terminated. */
        if (entry_len != path_len) {
            continue;
        }
        if (memcmp(entry_path, uri, path_len) != 0) {
            continue;
        }

        /* Path matches. */
        path_matched = true;

        if (s_dispatch[i].method == method) {
            *out_handler = s_dispatch[i].handler;
            return BB_DISPATCH_API_HIT;
        }
    }

    return path_matched ? BB_DISPATCH_API_METHOD_MISMATCH : BB_DISPATCH_API_MISS;
}

size_t bb_dispatch_api_count(void)
{
    return s_count;
}
