#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse result codes for provisioning form body
typedef enum {
    BB_PROV_PARSE_OK = 0,
    BB_PROV_PARSE_EMPTY_BODY,
    BB_PROV_PARSE_SSID_REQUIRED,
} bb_prov_parse_result_t;

// Parse a URL-encoded provisioning POST body into ssid/pass.
// body_len <= 0 → BB_PROV_PARSE_EMPTY_BODY.
// Missing/empty ssid → BB_PROV_PARSE_SSID_REQUIRED.
// body need not be null-terminated; function treats body_len as authoritative.
bb_prov_parse_result_t bb_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size);

// ============================================================================
// ESP-IDF-SPECIFIC API — provisioning lifecycle
// ============================================================================

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "http_server.h"

// Start HTTP server in provisioning mode.
// Registers: POST /save, OPTIONS /* (CORS preflight), GET /* (captive-portal redirect).
// Caller's prov_ui_routes_fn registers GET / and any static assets (favicon, css, logo).
esp_err_t bb_prov_start(bb_http_app_routes_fn prov_ui_routes_fn);

// Switch from provisioning mode to normal mode (unregister prov handlers, call app routes).
void bb_prov_switch_to_normal(bb_http_app_routes_fn app_routes_fn);

#endif

#ifdef __cplusplus
}
#endif
