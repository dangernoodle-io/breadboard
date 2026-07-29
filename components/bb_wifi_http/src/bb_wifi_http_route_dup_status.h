#pragma once
// bb_wifi_http_route_dup_status — pure bb_err_t -> bb_err_t mapper for a
// single bb_wifi_routes_init() route-registration attempt
// (platform/espidf/bb_wifi_http/bb_wifi_http_routes.c). No FreeRTOS/ESP-IDF/
// httpd types -- host-testable in isolation, mirroring
// bb_wifi_http_apply_status.h.
//
// bb_wifi_routes_init() registers three routes (GET /api/wifi, POST
// /api/wifi/scan, and — CONFIG_BB_WIFI_RECONFIGURE only — PATCH /api/wifi)
// sharing the SAME "/api/*" (method,path) space as bb_wifi_prov's own scan
// route. A duplicate (method,path) registration there is first-registration-
// wins, not a genuine failure — see bb_wifi_prov.c's own dup handling for the
// precedent this mirrors (B1-1259). The ESP-IDF-only caller
// (bb_wifi_routes_init()) delegates the resulting bb_err_t -> "does this
// abort the whole init bundle" decision to
// bb_wifi_http_route_register_outcome() here, so the host test suite
// exercises every branch of that mapping even though the caller's own inputs
// (a live bb_http_handle_t + esp_wifi.h) cannot be host-compiled.

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pure bb_err_t -> bb_err_t mapping for a single route-registration attempt:
//   BB_ERR_INVALID_STATE -> BB_OK   (duplicate (method,path); another
//                                     component already serves this path,
//                                     first-registration-wins — tolerated,
//                                     not a bundle-aborting failure)
//   any other value       -> unchanged (BB_OK stays BB_OK; a genuine failure
//                                     such as BB_ERR_NO_SPACE still aborts
//                                     the whole bb_wifi_routes_init() bundle
//                                     — see that function's own doc comment
//                                     for why)
bb_err_t bb_wifi_http_route_register_outcome(bb_err_t rc);

#ifdef __cplusplus
}
#endif
