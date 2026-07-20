#pragma once
// bb_wifi_http_apply_status — pure bb_err_t -> HTTP status mapper for the
// PATCH /api/wifi apply-error path (wifi_patch_handler,
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c). No FreeRTOS/ESP-IDF/
// httpd types -- host-testable in isolation, mirroring cache_route_status.h.
//
// The ESP-IDF-only handler (platform/espidf/bb_wifi_http/bb_wifi_http_routes.c)
// calls bb_data_apply() and delegates the resulting bb_err_t -> HTTP-status
// decision to bb_wifi_http_status_for_apply_rc() here, so Coveralls sees and
// the host test suite exercises every branch of that mapping even though the
// caller's own inputs (a live bb_data binding + httpd_req_t) cannot be
// host-compiled.
//
// This was the finding pinned by B1-1090 (test-only mirror drifting from
// production): the mapping list here is the ONLY copy -- production and the
// host tests both call this same function.

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pure bb_err_t -> HTTP status mapping for the apply-error path:
//   BB_ERR_VALIDATION       -> 400 (domain reject, e.g. bad ssid/password)
//   BB_ERR_PARSE_GRAMMAR     -> 400 (malformed request body)
//   BB_ERR_PARSE_INCOMPLETE  -> 400 (truncated request body)
//   BB_ERR_INVALID_ARG       -> 400 (bad recv/argument)
//   BB_ERR_UNSUPPORTED       -> 400 (unsupported request shape)
//   any other non-BB_OK      -> 500 (internal error)
//   BB_OK                    -> 202 (accepted, rebooting to try wifi)
int bb_wifi_http_status_for_apply_rc(bb_err_t rc);

#ifdef __cplusplus
}
#endif
