#pragma once
// Private interface shared by bb_update_check_common.c and the per-platform
// ports / route handlers. Not for external consumers.
#include "bb_core.h"
#include "bb_update_check.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

#ifdef BB_UPDATE_CHECK_TESTING
// Reset all state so a test can start clean. Does NOT touch bb_event or
// bb_mdns global state (callers reset those separately).
void bb_update_check_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
