#pragma once
// Private interface shared by bb_update_check_common.c and the per-platform
// ports / route handlers. Not for external consumers.

// Single source of truth for the update-check event topic name.
#define BB_UPDATE_CHECK_TOPIC "update.available"
#include "bb_cache.h"
#include "bb_core.h"
#include "bb_json.h"
#include "bb_update_check.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Canonical owned snapshot for the update.available bb_cache topic.
// Shared between SSE (bb_update_check_common.c) and REST (bb_update_check_espidf.c).
// Included by test/test_host/test_bb_cache_fidelity.c.
// ---------------------------------------------------------------------------
typedef struct {
    char    current[24];       // matches bb_update_check_status_t.current
    char    latest[24];        // matches bb_update_check_status_t.latest
    char    download_url[256]; // URL_MAX
    bool    available;
    int64_t ts;                // wall-clock seconds at publish time
    bool    last_check_ok;
    bool    enabled;
    char    outcome[24];       // outcome_str result; longest = "check_on_apply" (14)
    int64_t last_check_ts;     // epoch-s from last_check_us; 0 = omit
} bb_update_snap_t;

// Serializer — signature matches bb_cache_serialize_fn.
// Emits the canonical union shape for both SSE and REST.
void bb_update_serialize(bb_json_t obj, const void *snap);

#ifdef __cplusplus
extern "C" {
#endif

// Status accessor for the route handler (already covered by the public API,
// but exposed here so the ESP-IDF route impl doesn't pull bb_update_check.h
// twice).
// Already in the public header.

// Synchronous one-shot check. Called by the worker task / bb_timer callback,
// AND directly by host tests. Performs:
//   bb_http_client_get -> parser -> semver compare -> publish on transition.
// Updates s_status atomically. Returns BB_OK on transport success (even if
// HTTP status is non-200), BB_ERR_INVALID_STATE if URL is not set,
// BB_ERR_INVALID_ARG if init hasn't run.
bb_err_t bb_update_check_run_one(void);

// HTTP handler functions for GET/POST /api/update/check/config.
// Defined in bb_update_check_common.c so they compile on host (for testing).
// Route registration is performed by the platform port (bb_update_check_espidf.c).
#include "bb_http.h"
bb_err_t bb_update_check_config_get_handler(bb_http_request_t *req);
bb_err_t bb_update_check_config_post_handler(bb_http_request_t *req);

// Accessors for the route descriptors (static rodata in bb_update_check_common.c).
const bb_route_t *bb_update_check_config_get_route(void);
const bb_route_t *bb_update_check_config_post_route(void);

#ifdef BB_UPDATE_CHECK_TESTING
// Reset all state so a test can start clean. Does NOT touch bb_event or
// bb_mdns global state (callers reset those separately).
void bb_update_check_reset_for_test(void);

// Inject or clear the in-flight guard so host tests can exercise the
// skip-on-concurrent-kick path without a real FreeRTOS task.
// On ESP-IDF this toggles s_check_in_flight; on host it toggles a mirrored
// flag that the host kick() stub respects.
void bb_update_check_set_in_flight_for_test(bool in_flight);
bool bb_update_check_get_in_flight_for_test(void);

#ifndef ESP_PLATFORM
// Return the current holder of the host OTA claim stub (NULL = free).
// Used by tests that verify claim acquire/release around the apply path.
const char *bb_update_check_ota_claim_holder_for_test(void);
#endif
#endif

#ifdef __cplusplus
}
#endif
