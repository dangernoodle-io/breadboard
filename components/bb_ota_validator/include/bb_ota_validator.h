#pragma once
#include <stdbool.h>
#include "bb_nv.h"
#ifdef __cplusplus
extern "C" {
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
