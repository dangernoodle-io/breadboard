#pragma once

/**
 * @brief Backend-agnostic DELETE /api/diag/storage route over the
 * bb_storage facade — works for any registered backend (nvs today,
 * ram/rtc/a future sdcard) with no new route component per backend.
 */

// bb_storage_http — backend-agnostic DELETE route for bb_storage (B1-757).
//
// Registers DELETE /api/diag/storage on the shared bb_http_server. Rehomed
// from the old DELETE /api/nvs (platform/espidf/bb_nv/bb_nv_delete_routes.c),
// which was welded to the "nvs" backend and required bb_diag to reach into
// bb_nv's PRIVATE include dir (target_include_directories(... PRIVATE
// .../bb_nv)) just to call bb_nv_delete_routes_init — a build-system-level
// component-boundary violation.
//
// This component sits on the bb_storage FACADE only — REQUIRES bb_core
// bb_storage, never bb_storage_nvs — so any backend registered with
// bb_storage (nvs today, ram/rtc/a future sdcard) works through this SAME
// route with no new route component per backend.
//
// Body schema:
//   {
//     "backend":   <string>,                      // optional, default "nvs"
//     "namespace": <string | array-of-strings>,    // required
//     "key":       <string>,                       // optional; forbidden with array namespace
//     "confirm":   true,                           // required; else 412
//     "wipe_wifi": true                             // required when namespace is/includes the
//                                                    // wifi-creds namespace bb_settings owns
//   }
//
// Guard policy:
//   - "confirm": true required (412 without it).
//   - a namespace bb_settings identifies as the wifi-creds namespace (see
//     bb_settings_ns_is_wifi_creds) additionally requires "wipe_wifi": true,
//     because that namespace holds wifi credentials. Note: if
//     CONFIG_BB_NV_CREDS_RTC_BACKUP is enabled, credentials are
//     automatically restored from the RTC mirror on the next boot if the
//     mirror is valid.
//   - "key" with an array namespace returns 400 (ambiguous).
//   - a backend that does not implement namespace-level erase
//     (bb_storage_vtable_t.erase_namespace == NULL) surfaces
//     BB_ERR_UNSUPPORTED as 501 — never a silent no-op on a destructive
//     request.
//
// Responses:
//   200 {"deleted": [...]}  — list of namespaces (and key) cleared
//   400 missing/invalid namespace, or key+array namespace combo
//   412 missing confirm / missing wipe_wifi when the wifi-creds namespace is in scope
//   500 storage erase operation failed
//   501 backend does not support namespace-level erase

#include "bb_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registry hook — registers DELETE /api/diag/storage.
// bbtool:init tier=regular fn=bb_storage_http_routes_init server=true
bb_err_t bb_storage_http_routes_init(bb_http_handle_t server);

#ifdef BB_STORAGE_HTTP_TESTING
// Expose the handler for host unit tests.
bb_err_t bb_storage_http_delete_handler_for_test(bb_http_request_t *req);
#endif /* BB_STORAGE_HTTP_TESTING */

#ifdef __cplusplus
}
#endif
