#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// URL-decode a named field from a URL-encoded body (e.g., "field=value&...")
void bb_url_decode_field(const char *body, const char *field, char *out, size_t out_size);

// ============================================================================
// PORTABLE HTTP SERVER API — available on all platforms (ESP-IDF, Arduino, host)
// ============================================================================

#include "nv_config.h"  // Provides bb_err_t and error code definitions

// Opaque handle to the HTTP server.
typedef void *bb_http_handle_t;

// Opaque request object; impl-specific underneath.
typedef void *bb_http_request_t;

// HTTP methods
typedef enum {
    BB_HTTP_GET,
    BB_HTTP_POST,
} bb_http_method_t;

// Route handler — portable signature. Return BB_OK on success.
typedef bb_err_t (*bb_http_handler_fn)(bb_http_request_t *req);

// Registration callback (called inside bb_http_server_start).
// Invoked with the server handle so routes can be registered.
typedef bb_err_t (*bb_http_app_routes_fn)(bb_http_handle_t server);

// Register a route from inside bb_http_app_routes_fn.
bb_err_t bb_http_register_route(bb_http_handle_t server,
                                bb_http_method_t method,
                                const char *path,
                                bb_http_handler_fn handler);

// Response helpers — usable inside a handler. MVP: fixed-size body, no streaming.
bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code);
bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value);
bb_err_t bb_http_resp_send(bb_http_request_t *req, const char *body, size_t len);

// Request accessors — MVP: read the body as a single buffer.
int bb_http_req_body_len(bb_http_request_t *req);
int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size);

// Lifecycle — portable on all platforms
bb_err_t bb_http_server_start(bb_http_app_routes_fn routes_fn);
bb_err_t bb_http_server_stop(void);
bb_http_handle_t bb_http_server_get_handle(void);

// Poll the server for new connections (Arduino backend only; no-op on ESP-IDF).
void bb_http_server_poll(void);

// ============================================================================
// ESP-IDF-SPECIFIC API — low-level helpers
// ============================================================================

#ifdef ESP_PLATFORM
#include "esp_err.h"

// Ensure the HTTP server is started (low-level helper; prefer bb_http_server_start).
// Used by provisioning and other advanced features.
esp_err_t bb_http_server_ensure_started(void);

#endif

#ifdef __cplusplus
}
#endif
