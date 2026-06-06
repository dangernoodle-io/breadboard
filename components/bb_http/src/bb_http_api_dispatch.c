#include "bb_http_api_dispatch.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal route entry
 * ---------------------------------------------------------------------------*/
typedef struct {
    bb_http_method_t  method;
    const char       *path;
    bb_http_handler_fn handler;
} bb_api_dispatch_entry_t;

/* ---------------------------------------------------------------------------
 * File-scope state — no heap, no ESP, s_ prefix per house rules
 * ---------------------------------------------------------------------------*/
static bb_api_dispatch_entry_t s_dispatch[BB_API_DISPATCH_CAP];
static size_t                  s_count;

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

void bb_api_dispatch_reset(void)
{
    memset(s_dispatch, 0, sizeof(s_dispatch));
    s_count = 0;
}

bb_err_t bb_api_dispatch_add(bb_http_method_t method, const char *path,
                              bb_http_handler_fn handler)
{
    if (s_count >= BB_API_DISPATCH_CAP) {
        return BB_ERR_NO_SPACE;
    }
    s_dispatch[s_count].method  = method;
    s_dispatch[s_count].path    = path;
    s_dispatch[s_count].handler = handler;
    s_count++;
    return BB_OK;
}

bb_api_dispatch_result_t bb_api_dispatch_lookup(bb_http_method_t method,
                                                const char *uri,
                                                bb_http_handler_fn *out_handler)
{
    if (uri == NULL || out_handler == NULL) {
        return BB_API_DISPATCH_MISS;
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
            return BB_API_DISPATCH_HIT;
        }
    }

    return path_matched ? BB_API_DISPATCH_METHOD_MISMATCH : BB_API_DISPATCH_MISS;
}

size_t bb_api_dispatch_count(void)
{
    return s_count;
}
