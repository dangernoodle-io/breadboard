#pragma once
// bb_wifi_http_patch_scratch — PATCH /api/wifi's scratch-acquire fail-closed
// insertion point (B1-1287 httpd worker stack-exhaustion fix), extracted so
// the REAL bb_data_scratch_acquire()/bb_http_serialize_send_error() call
// chain is host-testable directly. platform/espidf/bb_wifi_http/
// bb_wifi_http_routes.c (wifi_patch_handler's home) is ESP-IDF-only --
// includes <esp_wifi.h> unconditionally and its component PRIV_REQUIREs
// esp_wifi -- so that TU can never be host-compiled, and this branch was
// previously exercised only on hardware (B1-1285). No FreeRTOS/ESP-IDF
// types here -- host-testable in isolation, mirroring
// bb_wifi_http_route_dup_status.h/bb_wifi_http_apply_status.h. Unlike those
// two (pure bb_err_t -> bb_err_t mappers over an already-computed result),
// this fn performs the acquire and the send_error call itself: a host test
// exercises the real insertion-point logic, not a hand-copied predicate over
// its outcome.

#include "bb_core.h"
#include "bb_data.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Acquires bb_data's shared scratch pair into `*out`. On BB_OK, no response
// is sent (the caller proceeds to use `*out`). On any other value —
// BB_ERR_INVALID_STATE (scratch pair already held by another in-flight
// caller; the "one httpd worker task, one handler in flight" invariant is
// broken) or BB_ERR_INVALID_ARG (out is NULL) — this fn sends the same
// {"error":"internal error"} 500 body wifi_patch_handler's own fail-closed
// branch sent inline before this extraction, then returns that same rc
// unchanged. Callers just `return rc;` on a non-BB_OK result.
bb_err_t bb_wifi_http_patch_acquire_scratch(bb_http_request_t *req, bb_data_scratch_t *out);

#ifdef __cplusplus
}
#endif
