#pragma once
#include <stdbool.h>
#include "bb_nv.h"
#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "bb_http.h"

// Initialize the validator state machine. Performs boot-time checks:
//   - Queries partition state via esp_ota_get_running_partition + esp_ota_get_state_partition.
//   - If state is ESP_OTA_IMG_PENDING_VERIFY and the other slot lacks a valid app
//     (rollback target unsafe), marks valid immediately.
//   - Otherwise sets internal pending flag.
// Registers POST /api/ota/mark-valid on the server.
bb_err_t bb_ota_validator_init(bb_http_handle_t server);

#endif

// Push a mark-valid signal. No-op if not pending or already marked.
// Returns BB_OK on success, BB_ERR_INVALID_STATE if not pending / already marked.
// Side effects: esp_ota_mark_app_valid_cancel_rollback + bb_nv_config_reset_boot_count.
bb_err_t bb_ota_mark_valid(const char *reason);

// True only between a pending-detection at init and a successful mark_valid.
bool bb_ota_is_pending(void);

#ifdef __cplusplus
}
#endif
