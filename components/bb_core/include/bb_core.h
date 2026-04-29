#pragma once

// Foundational workspace types: error codes + opaque HTTP handles.
// Every bb_* component REQUIRES bb_core for these types.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Error type and codes
// ---------------------------------------------------------------------------
// bb_err_t is a typedef for esp_err_t under ESP-IDF and int elsewhere —
// values pass through unchanged. The constants below alias to ESP_OK /
// ESP_ERR_* under ESP-IDF so esp_* return values compare correctly without
// translation at component boundaries.
#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t bb_err_t;
#define BB_OK                   ESP_OK
#define BB_ERR_INVALID_ARG      ESP_ERR_INVALID_ARG
#define BB_ERR_NOT_FOUND        ESP_ERR_NVS_NOT_FOUND
#define BB_ERR_NOT_INITIALIZED  ESP_ERR_NVS_NOT_INITIALIZED
#define BB_ERR_NO_SPACE         ESP_ERR_NO_MEM
#define BB_ERR_INVALID_STATE    ESP_ERR_INVALID_STATE
#else
typedef int bb_err_t;
#define BB_OK                   0
#define BB_ERR_INVALID_ARG      1
#define BB_ERR_NOT_FOUND        2
#define BB_ERR_NOT_INITIALIZED  3
#define BB_ERR_NO_SPACE         4
#define BB_ERR_INVALID_STATE    5
#endif

// ---------------------------------------------------------------------------
// Opaque HTTP handles
// ---------------------------------------------------------------------------
// bb_http_handle_t identifies the HTTP server instance; bb_http_request_t
// identifies an in-flight request. Both are typedef'd to void * so that
// non-route components (e.g. bb_mdns implementing a registry init_fn that
// ignores the server arg) can use the type without REQUIRES bb_http.

typedef void *bb_http_handle_t;
typedef void *bb_http_request_t;

#ifdef __cplusplus
}
#endif
