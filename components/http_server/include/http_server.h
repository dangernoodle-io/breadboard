#pragma once

#include <stddef.h>

// URL-decode a named field from a URL-encoded body (e.g., "field=value&...")
void bsp_url_decode_field(const char *body, const char *field, char *out, size_t out_size);

typedef enum {
    BSP_PROV_PARSE_OK = 0,
    BSP_PROV_PARSE_EMPTY_BODY,
    BSP_PROV_PARSE_SSID_REQUIRED,
} bsp_prov_parse_result_t;

// Parse a URL-encoded provisioning POST body into ssid/pass.
// body_len <= 0 → BSP_PROV_PARSE_EMPTY_BODY.
// Missing/empty ssid → BSP_PROV_PARSE_SSID_REQUIRED.
// body need not be null-terminated; function treats body_len as authoritative.
bsp_prov_parse_result_t bsp_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_http_server.h"

// Callback to register app-specific routes after provisioning/on normal boot
typedef esp_err_t (*bsp_http_app_routes_fn)(httpd_handle_t server);

// Start HTTP server with provisioning form handlers (GET / and POST /save)
esp_err_t bsp_http_server_start_prov(void);

// Start HTTP server and register app routes
esp_err_t bsp_http_server_start(bsp_http_app_routes_fn app_routes_fn);

// Switch from provisioning mode to normal mode (unregister prov handlers, call app routes)
void bsp_http_server_switch_to_normal(bsp_http_app_routes_fn app_routes_fn);

// Stop HTTP server
esp_err_t bsp_http_server_stop(void);

// Get current server handle (for internal use by app routes)
httpd_handle_t bsp_http_server_get_handle(void);

#endif
