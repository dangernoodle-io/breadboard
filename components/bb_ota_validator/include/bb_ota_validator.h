#pragma once

#include <stdbool.h>
#include "bb_nv.h"

#ifdef __cplusplus
extern "C" {
#endif

// OTA validator strategy: consumer-provided implementation of pending check
// and valid marking. NULL strategy in registration uses the built-in default.
typedef struct {
    bool (*is_pending)(void);
    bb_err_t (*mark_valid)(const char *reason);
} bb_ota_validator_strategy_t;

// Default implementations. On ESP-IDF, these check the partition state and
// atomic pending flag. On non-ESP platforms, these are stubs returning
// false / BB_ERR_INVALID_STATE respectively.
bool bb_ota_default_is_pending(void);
bb_err_t bb_ota_default_mark_valid(const char *reason);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "bb_http.h"

// Register POST /api/ota/mark-valid route on the given server.
// If strategy is NULL, uses the built-in default (bb_ota_default_*).
// Handler returns 200 with {"status":"valid"} on success,
// 409 {"error":"not pending"} if already valid / no rollback in progress,
// 500 {"error":"internal"} on unexpected failure.
esp_err_t bb_ota_register_validator_route(bb_http_handle_t server,
                                          const bb_ota_validator_strategy_t *strategy);

#endif

#ifdef __cplusplus
}
#endif
