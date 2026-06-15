#pragma once
// bb_nv_delete_routes — NVS delete HTTP route (B1-290).
//
// Registers one DELETE endpoint on the bb_http server:
//   DELETE /api/nvs   (JSON body)
//
// Body schema:
//   {
//     "namespace": <string | array-of-strings>,  // required
//     "key":       <string>,                      // optional; forbidden with array namespace
//     "confirm":   true,                          // required; else 412
//     "wipe_wifi": true                           // required when namespace is/includes "bb_cfg"
//   }
//
// All requests require "confirm": true to prevent accidental wipes (412 without
// it). When namespace is (or includes) "bb_cfg", "wipe_wifi": true is also
// required because bb_cfg holds wifi credentials.
//
// WiFi-creds safety note: erasing "bb_cfg" removes wifi_ssid / wifi_pass.
// When CONFIG_BB_NV_CREDS_RTC_BACKUP is enabled, credentials are automatically
// restored from the RTC mirror on the next boot if the mirror is valid. Without
// the RTC backup a headless board may enter provisioning mode after reboot.
//
// Use an array namespace to clear multiple namespaces in one call. This covers
// the "reset telemetry" case: ["bb_mqtt","bb_sink_http","bb_pub"].
//
// Called from bb_diag_routes_init. Portable: no ESP-IDF-specific includes.

#include "bb_core.h"
#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the NVS delete route on the given server handle.
// Returns BB_OK on success, or the route registration error.
bb_err_t bb_nv_delete_routes_init(bb_http_handle_t server);

#ifdef BB_NV_DELETE_ROUTES_TESTING
// Expose handler for host unit tests.
bb_err_t bb_nv_delete_handler_for_test(bb_http_request_t *req);
#endif /* BB_NV_DELETE_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
