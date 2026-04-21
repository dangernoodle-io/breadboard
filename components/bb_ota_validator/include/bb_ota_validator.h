#pragma once

#include <stdbool.h>
#include "bb_nv.h"

#ifdef __cplusplus
extern "C" {
#endif

// Manually mark the running OTA slot valid (cancel rollback).
// Returns BB_OK on success; BB_ERR_INVALID_STATE if the slot is not
// PENDING_VERIFY (already valid, or not an OTA boot).
bb_err_t bb_ota_mark_valid_manual(void);

// True if the running slot is PENDING_VERIFY and has not yet been marked valid.
bool bb_ota_is_pending(void);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "bb_http.h"

// Register POST /api/ota/mark-valid route on the given server.
// Handler returns 200 with {"status":"valid"} on success,
// 409 {"error":"not pending"} if already valid / no rollback in progress,
// 500 {"error":"internal"} on unexpected failure.
esp_err_t bb_ota_register_validator_route(bb_http_handle_t server);

#endif

#ifdef __cplusplus
}
#endif
