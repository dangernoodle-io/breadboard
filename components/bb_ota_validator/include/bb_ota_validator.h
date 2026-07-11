#pragma once
#include <stdbool.h>
#include "bb_core.h"
#ifdef __cplusplus
extern "C" {
#endif

// Push a mark-valid signal. No-op if not pending or already marked.
// Returns BB_OK on success, BB_ERR_INVALID_STATE if not pending / already marked.
// Side effects: esp_ota_mark_app_valid_cancel_rollback + bb_system_boot_count_reset.
bb_err_t bb_ota_mark_valid(const char *reason);

// True only between a pending-detection at init and a successful mark_valid.
bool bb_ota_is_pending(void);

// Query whether the running firmware partition state is valid.
// Returns true if the running partition is marked as valid in the OTA metadata.
// On non-ESP platforms or if the running partition is unprovisioned/factory, returns false.
// This state is immutable post-boot and reflects the persistent OTA partition state
// (unlike bb_ota_is_pending, which tracks in-process validation progress).
bool bb_ota_is_validated(void);

// True if the non-running OTA slot is in state ABORTED or INVALID — the bootloader's
// rollback marker. ABORTED is set when a PENDING_VERIFY image timed out and was rolled
// back by the bootloader. INVALID is set when the app explicitly called
// esp_ota_mark_app_invalid_rollback_and_reboot. Both indicate a previous OTA attempt
// was rolled back; either state means a rollback occurred.
// Returns false on host/Arduino (no OTA partitions) and when the other slot is absent
// or in any other state.
bool bb_ota_rolled_back(void);

// Register a callback to be invoked after bb_ota_mark_valid() succeeds.
// Only one callback slot; last registration wins. Pass NULL to clear.
// Called from mark_valid_internal — may be on the HTTP handler task or any caller's stack.
// Must be set before bb_ota_mark_valid() is called (i.e., at component init time).
void bb_ota_validator_set_on_validated(void (*cb)(void));

#ifdef ESP_PLATFORM
#include "bb_http_server.h"

/* Reserve route-table slots for bb_ota_validator before the HTTP server starts. */
// bbtool:init tier=pre_http fn=bb_ota_validator_reserve_routes
bb_err_t bb_ota_validator_reserve_routes(void);

/* Registers POST /api/update/mark-valid, GET /api/update/partitions,
 * POST /api/update/recover with an existing httpd instance. */
// bbtool:init tier=regular fn=bb_ota_validator_init server=true
bb_err_t bb_ota_validator_init(bb_http_handle_t server);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
