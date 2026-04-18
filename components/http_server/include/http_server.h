#pragma once

#include <stddef.h>

// URL-decode a named field from a URL-encoded body (e.g., "field=value&...")
void bsp_url_decode_field(const char *body, const char *field, char *out, size_t out_size);

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
