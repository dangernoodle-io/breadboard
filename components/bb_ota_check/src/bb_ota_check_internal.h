#pragma once
// Private interface shared by bb_ota_check_common.c and the per-platform
// ports / route handlers. Not for external consumers.

#include "bb_cache.h"
#include "bb_core.h"
#include "bb_ota_check.h"
// BB_OTA_CHECK_TOPIC + bb_ota_check_snap_t + bb_ota_check_wire_desc
// (B1-1045 PR-2 wire primitive) now live in the public bb_ota_check_wire.h
// -- moved there so the wire descriptor can reuse the snapshot struct
// directly rather than a widened parallel one (see bb_ota_check_wire.h
// banner). bb_ota_check_serialize() (the legacy bb_json bb_cache
// serializer this header used to declare) was deleted in B1-1053 PR3 --
// GET /api/update/status now renders via bb_data_render() against
// bb_ota_check_wire_desc instead (see bb_ota_check_emit_status_json(),
// bb_ota_check_common.c).
#include "bb_ota_check_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Status accessor for the route handler (already covered by the public API,
// but exposed here so the ESP-IDF route impl doesn't pull bb_ota_check.h
// twice).
// Already in the public header.

// True once bb_ota_check_init() has completed successfully. Backs the
// on-demand kick() / run_blocking() entry guards on ESP-IDF, decoupling them
// from the (autoregister-gated) periodic timer's s_timer handle.
bool bb_ota_check_is_initialized(void);

// Synchronous one-shot check. Called by the worker task / bb_timer callback,
// AND directly by host tests. Performs:
//   bb_http_client_get -> parser -> semver compare -> publish on transition.
// Updates s_status atomically. Returns BB_OK on transport success (even if
// HTTP status is non-200), BB_ERR_INVALID_STATE if URL is not set,
// BB_ERR_INVALID_ARG if init hasn't run.
bb_err_t bb_ota_check_run_one(void);

// HTTP handler functions for GET/POST /api/update/check/config.
// Defined in bb_ota_check_common.c so they compile on host (for testing).
// Route registration is performed by the platform port (bb_ota_check_espidf.c).
#include "bb_http.h"
bb_err_t bb_ota_check_config_get_handler(bb_http_request_t *req);
bb_err_t bb_ota_check_config_post_handler(bb_http_request_t *req);

// B1-859: binds the "ota_check_config" bb_data key (production gather/apply
// hooks backing POST /api/update/config) against the bb_data binding table.
// Portable -- no bb_http_handle_t server dependency. MUST be called before
// bb_ota_check_config_post_handler() is ever driven (production: from
// bb_ota_check_register_init(); host tests: directly after
// bb_data_test_reset()).
bb_err_t bb_ota_check_config_bind(void);

// Accessors for the route descriptors (static rodata in bb_ota_check_common.c).
const bb_route_t *bb_ota_check_config_get_route(void);
const bb_route_t *bb_ota_check_config_post_route(void);

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-1) -- co-located JSON
// Schema docs/validation table for the POST /api/update/config request
// descriptor (bb_ota_check_common.c's file-scope s_config_desc), same
// #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar (B1-1059 PR-2a).
// BB_SERIALIZE_META_HOST is a host-only define (set by the PlatformIO
// native env; see platformio.ini) -- NEVER set by the ESP-IDF/device
// build, so these two declarations (and their definitions in
// bb_ota_check_common.c) compile to nothing on-device. The desc itself is
// file-scope static (no companion _wire_priv.h for this request shape), so
// a for-test accessor exposes it rather than an extern -- same "_for_test"
// naming convention as the BB_OTA_CHECK_TESTING-gated fns below, and
// double-gated on BB_OTA_CHECK_TESTING for the same reason (test-only
// surface, same posture as bb_storage_http.h's analogous factory_reset
// accessor).
// Resolve the include before the guard below instead of relying on a
// transitive include having already defined BB_SERIALIZE_META_SHIP -- the
// header itself is header-only/zero-code when off, so including it here is
// inert either way.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP) && defined(BB_OTA_CHECK_TESTING)

const bb_serialize_desc_t *bb_ota_check_config_desc_for_test(void);
extern const bb_serialize_desc_meta_t bb_ota_check_config_meta;
#endif /* defined(BB_SERIALIZE_META_HOST) && defined(BB_OTA_CHECK_TESTING) */

#ifdef BB_OTA_CHECK_TESTING
// Reset all state so a test can start clean. Does NOT touch bb_data or
// bb_mdns global state (callers reset those separately).
void bb_ota_check_reset_for_test(void);

// Inject or clear the in-flight guard so host tests can exercise the
// skip-on-concurrent-kick path without a real FreeRTOS task.
// On ESP-IDF this toggles s_check_in_flight; on host it toggles a mirrored
// flag that the host kick() stub respects.
void bb_ota_check_set_in_flight_for_test(bool in_flight);
bool bb_ota_check_get_in_flight_for_test(void);

#ifndef ESP_PLATFORM
// Return the current holder of the host OTA claim stub (NULL = free).
// Used by tests that verify claim acquire/release around the apply path.
const char *bb_ota_check_ota_claim_holder_for_test(void);
#endif

// Headroom worst-case fixture support (BB_OTA_CHECK_RENDER_BUF_BYTES proof,
// test_emit_status_json_render_buf_headroom). `current` has no public
// setter -- it is meant to reflect the running firmware's own version,
// fixed at bb_ota_check_init() time from bb_system_get_version() -- so a
// genuine max-length fixture needs to inject it directly.
void bb_ota_check_set_current_for_test(const char *version);

// Headroom worst-case fixture support: directly overrides s_last_publish_ts
// (the "ts" wire field) and s_status.last_check_us (the microsecond source
// of the "last_check_ts" wire field) so a fixture can drive both int64
// fields toward their full-width extremes without waiting on wall-clock
// time.
void bb_ota_check_set_ts_for_test(int64_t publish_ts_s, int64_t last_check_us);
#endif

#ifdef __cplusplus
}
#endif
