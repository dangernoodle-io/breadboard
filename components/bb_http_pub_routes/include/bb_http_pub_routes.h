// bb_http_pub_routes — GET + PATCH /api/httppub HTTP routes.
//
// GET  /api/httppub  → {base, path_tmpl, qos, enabled, ca_set, cert_set, key_set}
//                      Secrets never returned; only presence reported.
// PATCH /api/httppub → parse JSON body, persist fields to NVS "bb_http_pub";
//                      refreshes cached cfg; returns 204 on success.
//
// Registers at regular tier (order 6, CONFIG_BB_HTTP_PUB_ROUTES_AUTOREGISTER, default y).
//
// Host twin: platform/host/bb_http_pub_routes/bb_http_pub_routes_host.c
#pragma once
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET+PATCH /api/httppub with the HTTP server.
// Called automatically when CONFIG_BB_HTTP_PUB_ROUTES_AUTOREGISTER=y.
bb_err_t bb_http_pub_routes_init(bb_http_handle_t server);

#ifdef BB_HTTP_PUB_ROUTES_TESTING

// Reset route state for test isolation.
void bb_http_pub_routes_reset_for_test(void);

// Expose GET and PATCH handlers for test invocation.
bb_err_t bb_http_pub_routes_get_handler_for_test(bb_http_request_t *req);
bb_err_t bb_http_pub_routes_patch_handler_for_test(bb_http_request_t *req);

#endif /* BB_HTTP_PUB_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
