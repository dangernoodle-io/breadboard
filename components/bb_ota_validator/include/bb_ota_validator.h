#pragma once
#include <stdbool.h>
#include "bb_core.h"
#ifdef __cplusplus
extern "C" {
#endif

// Push a mark-valid signal. No-op if not pending or already marked.
// Returns BB_OK on success, BB_ERR_INVALID_STATE if not pending / already marked.
// Side effects: esp_ota_mark_app_valid_cancel_rollback + bb_nv_config_reset_boot_count.
bb_err_t bb_ota_mark_valid(const char *reason);

// True only between a pending-detection at init and a successful mark_valid.
bool bb_ota_is_pending(void);

// Query whether the running firmware partition state is valid.
// Returns true if the running partition is marked as valid in the OTA metadata.
// On non-ESP platforms or if the running partition is unprovisioned/factory, returns false.
// This state is immutable post-boot and reflects the persistent OTA partition state
// (unlike bb_ota_is_pending, which tracks in-process validation progress).
bool bb_ota_is_validated(void);

#ifdef __cplusplus
}
#endif
