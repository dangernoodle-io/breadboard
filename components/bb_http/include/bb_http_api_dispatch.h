#pragma once

#include <stddef.h>

#include "bb_core.h"
#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Dispatch table capacity.  Override at build time via Kconfig or -D.
 * ---------------------------------------------------------------------------*/
#ifdef CONFIG_BB_HTTP_API_DISPATCH_CAP
#define BB_API_DISPATCH_CAP CONFIG_BB_HTTP_API_DISPATCH_CAP
#else
#define BB_API_DISPATCH_CAP 64
#endif

/* ---------------------------------------------------------------------------
 * Result codes returned by bb_api_dispatch_lookup().
 * ---------------------------------------------------------------------------*/
typedef enum {
    BB_API_DISPATCH_HIT,             /* exact path+method match; *out_handler set */
    BB_API_DISPATCH_METHOD_MISMATCH, /* path exists, method differs → caller returns 405 */
    BB_API_DISPATCH_MISS,            /* no such path → caller returns 404 */
} bb_api_dispatch_result_t;

/* Clear the dispatch table (use in tests and re-init). */
void bb_api_dispatch_reset(void);

/* Append a route.
 * Returns BB_OK on success, or BB_ERR_NO_SPACE when the table is full.
 * Caller should log the overflow — it is non-fatal. */
bb_err_t bb_api_dispatch_add(bb_http_method_t method, const char *path,
                             bb_http_handler_fn handler);

/* Look up by method + URI.  Query strings (everything from '?' onward) are
 * stripped before comparison.  Matching is exact (not prefix).
 * Sets *out_handler on HIT; leaves *out_handler unchanged otherwise.
 * Handles NULL uri and NULL out_handler defensively (returns MISS). */
bb_api_dispatch_result_t bb_api_dispatch_lookup(bb_http_method_t method,
                                                const char *uri,
                                                bb_http_handler_fn *out_handler);

/* Return the number of routes currently in the table.  Useful for tests and
 * telemetry. */
size_t bb_api_dispatch_count(void);

#ifdef __cplusplus
}
#endif
