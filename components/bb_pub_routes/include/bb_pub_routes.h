// bb_pub_routes — GET /api/pub publisher status route.
//
// GET /api/pub → {interval_ms, topic_prefix, source_count, sink_count,
//                 last_publish_ok, last_publish_age_ms, published_ever}
//
// Exposes bb_pub's runtime status so the UI can distinguish "transport
// connected" from "actually publishing telemetry".
//
// Registers via bb_registry at regular tier (order 6).
// Auto-register gated by CONFIG_BB_PUB_ROUTES_AUTOREGISTER (default y).
//
// Host twin: platform/host/bb_pub_routes/bb_pub_routes_host.c
#pragma once
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET /api/pub with the HTTP server.
// Called automatically when CONFIG_BB_PUB_ROUTES_AUTOREGISTER=y.
bb_err_t bb_pub_routes_init(bb_http_handle_t server);

#ifdef BB_PUB_ROUTES_TESTING

// Reset route state for test isolation.
void bb_pub_routes_reset_for_test(void);

// Expose GET handler for test invocation.
bb_err_t bb_pub_routes_get_handler_for_test(bb_http_request_t *req);

#endif /* BB_PUB_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
