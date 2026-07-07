#pragma once

// bb_cache_routes — REST pull face over bb_cache (sibling to bb_cache_reactive's
// push face). ONE route: GET /api/cache?key=<key> returns the key's enveloped
// {"ts_ms":N,"data":{...}} snapshot straight from bb_cache_get_serialized().
//
// Query-param (not path-param) because bb_dispatch_api is exact-match only
// (components/bb_http_server/include/bb_dispatch_api.h) — a single "/api/cache"
// dispatch entry serves every key, filtered by ?key=. Mirrors the ?topic=
// idiom on GET /api/events (bb_event_routes).
//
// ESP-IDF only: the handler is httpd-backed and lives in
// platform/espidf/bb_cache_routes/bb_cache_routes.c. Host twin:
// platform/host/bb_cache_routes/bb_cache_routes_host.c.

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET /api/cache on server. Regular-tier init, called either via
// CONFIG_BB_CACHE_ROUTES_AUTOREGISTER or manually by the consumer.
bb_err_t bb_cache_routes_init(bb_http_handle_t server);

#ifdef BB_CACHE_ROUTES_TESTING
void bb_cache_routes_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
