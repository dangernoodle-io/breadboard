#pragma once

#include "nv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "cJSON.h"

// Extender callback. Invoked with the root cJSON object of the /api/info
// response. Implementations MAY add keys; they MUST NOT delete root or
// replace existing keys. Invoked inline on the httpd task — extenders are
// responsible for their own synchronization.
typedef void (*bb_info_extender_fn)(cJSON *root);

// Register an extender. Fixed capacity (4 slots). Must be called before
// bb_http_server_start; registering after start returns BB_ERR_INVALID_ARG.
// Returns BB_ERR_NO_SPACE if the table is full.
bb_err_t bb_info_register_extender(bb_info_extender_fn fn);

// Register GET /api/info on server. Base response merges bb_board_get_info
// and bb_wifi_get_info output; all registered extenders run after.
esp_err_t bb_info_register_routes(void *server);

#endif

#ifdef __cplusplus
}
#endif
