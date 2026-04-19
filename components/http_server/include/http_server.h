#pragma once

#include <stddef.h>

// URL-decode a named field from a URL-encoded body (e.g., "field=value&...")
void bb_url_decode_field(const char *body, const char *field, char *out, size_t out_size);

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

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_http_server.h"

// Callback to register app-specific routes after provisioning/on normal boot
typedef esp_err_t (*bb_http_app_routes_fn)(httpd_handle_t server);

// Start HTTP server in provisioning mode.
// breadboard registers: POST /save, OPTIONS /* (CORS preflight), GET /* (captive-portal redirect).
// Caller's prov_ui_routes_fn registers GET / and any static assets (favicon, css, logo) they want to serve.
esp_err_t bb_http_server_start_prov(bb_http_app_routes_fn prov_ui_routes_fn);

// Start HTTP server and register app routes
esp_err_t bb_http_server_start(bb_http_app_routes_fn app_routes_fn);

// Switch from provisioning mode to normal mode (unregister prov handlers, call app routes)
void bb_http_server_switch_to_normal(bb_http_app_routes_fn app_routes_fn);

// Stop HTTP server
esp_err_t bb_http_server_stop(void);

// Get current server handle (for internal use by app routes)
httpd_handle_t bb_http_server_get_handle(void);

#endif
