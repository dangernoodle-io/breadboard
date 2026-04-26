#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// URL-decode a named field from a URL-encoded body (e.g., "field=value&...")
void bb_url_decode_field(const char *body, const char *field, char *out, size_t out_size);

// ============================================================================
// PORTABLE HTTP SERVER API — available on all platforms (ESP-IDF, Arduino, host)
// ============================================================================

#include "bb_nv.h"  // Provides bb_err_t and error code definitions

// Opaque handle to the HTTP server.
typedef void *bb_http_handle_t;

// Opaque request object; impl-specific underneath.
typedef void *bb_http_request_t;

// HTTP methods
typedef enum {
    BB_HTTP_GET,
    BB_HTTP_POST,
    BB_HTTP_PATCH,
    BB_HTTP_PUT,
    BB_HTTP_DELETE,
    BB_HTTP_OPTIONS,
} bb_http_method_t;

// Route handler — portable signature. Return BB_OK on success.
typedef bb_err_t (*bb_http_handler_fn)(bb_http_request_t *req);

// Register a route on an already-started server.
bb_err_t bb_http_register_route(bb_http_handle_t server,
                                bb_http_method_t method,
                                const char *path,
                                bb_http_handler_fn handler);

// ============================================================================
// ROUTE DESCRIPTORS — OpenAPI metadata carrier for bb_openapi spec emission
// ============================================================================

// One response variant for a route. Terminated by {.status = 0} sentinel.
// All pointer fields must remain valid for the life of the server (static/rodata).
typedef struct {
    int         status;        // HTTP status code (200, 202, 400, ...); 0 = sentinel terminator
    const char *content_type;  // e.g. "application/json", "text/event-stream", "text/plain"
    const char *schema;        // JSON Schema fragment as string literal; NULL for status-only entries
    const char *description;   // human-readable one-liner
} bb_route_response_t;

// Full descriptor for a single route carrying OpenAPI metadata.
// All pointer fields must remain valid for the life of the server (static/rodata).
// The registry stores const bb_route_t * pointers — descriptor lifetime is the
// caller's responsibility (same convention as bb_http_asset_t).
typedef struct {
    bb_http_method_t          method;
    const char               *path;                  // e.g. "/api/stats"
    const char               *tag;                   // OpenAPI grouping tag, e.g. "mining"
    const char               *summary;               // one-line operation summary
    const char               *operation_id;          // optional; spec emitter derives name if NULL
    const char               *request_content_type;  // NULL if the route takes no body
    const char               *request_schema;        // JSON Schema fragment; NULL if no body
    const bb_route_response_t *responses;            // null-terminated by {.status = 0}
    bb_http_handler_fn        handler;
} bb_route_t;

// Register a described route. Equivalent to bb_http_register_route() plus
// adding the descriptor to a process-wide registry that bb_openapi walks.
// On registration failure the descriptor is NOT added to the registry.
bb_err_t bb_http_register_described_route(bb_http_handle_t server,
                                          const bb_route_t *route);

// Walker callback type for the route registry.
typedef void (*bb_route_walker_fn)(const bb_route_t *route, void *ctx);

// Iterate every registered descriptor in insertion order.
void   bb_http_route_registry_foreach(bb_route_walker_fn cb, void *ctx);

// Remove all descriptors from the registry (intended for tests).
void   bb_http_route_registry_clear(void);

// Add a descriptor to the registry without registering a live handler.
// Use for routes whose handler is registered separately (e.g. SSE endpoints
// that call httpd_register_uri_handler directly).  The descriptor will appear
// in bb_openapi spec output exactly like a fully-described route.
// Returns BB_OK on success; BB_ERR_INVALID_ARG if route is NULL.
bb_err_t bb_http_register_route_descriptor_only(const bb_route_t *route);

// Return the number of descriptors currently in the registry.
size_t bb_http_route_registry_count(void);

// Response helpers — usable inside a handler. MVP: fixed-size body, no streaming.
bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code);
bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value);
bb_err_t bb_http_resp_send(bb_http_request_t *req, const char *body, size_t len);

// Send an HTTP error response (status + plain-text message). Convenience for handlers.
bb_err_t bb_http_resp_send_err(bb_http_request_t *req, int status_code, const char *message);

// Request accessors — MVP: read the body as a single buffer.
int bb_http_req_body_len(bb_http_request_t *req);
int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size);

// CORS preflight configuration. Must be called before bb_http_server_start().
// Defaults: methods = "GET, POST, OPTIONS", headers = "Content-Type".
// Passing NULL restores the default. Strings must remain valid for server lifetime.
void bb_http_set_cors_methods(const char *methods);
void bb_http_set_cors_headers(const char *headers);

// Lifecycle — portable on all platforms
// Start the HTTP server. After return, use bb_http_server_get_handle() and call
// bb_http_register_route / bb_http_register_assets as needed.
bb_err_t bb_http_server_start(void);
bb_err_t bb_http_server_stop(void);
bb_http_handle_t bb_http_server_get_handle(void);

// Poll the server for new connections (Arduino backend only; no-op on ESP-IDF).
void bb_http_server_poll(void);

// Register common built-in routes: GET /api/version, POST /api/reboot, GET /api/scan.
// /api/version returns bb_system_get_version() as text/plain.
// /api/reboot returns {"status":"rebooting"} then calls bb_system_reboot()
//   after a short backend-specific delay.
// /api/scan returns cached bb_wifi scan results as JSON and kicks a background refresh.
bb_err_t bb_http_register_common_routes(bb_http_handle_t server);

// Static asset entry. Lifetime: all pointer fields must remain valid for
// the life of the server registration (typically static/rodata).
typedef struct {
    const char    *path;       // e.g. "/theme.css"
    const char    *mime;       // e.g. "text/css"
    const char    *encoding;   // optional; NULL or "gzip"
    const uint8_t *data;
    size_t         len;
} bb_http_asset_t;

// Register a table of static GET assets on an already-started server.
// Each entry becomes a GET handler emitting `data` with Content-Type=mime
// and (if encoding!=NULL) Content-Encoding=encoding, plus a sensible
// Cache-Control. Returns BB_OK on success; first registration failure aborts.
bb_err_t bb_http_register_assets(bb_http_handle_t server,
                                 const bb_http_asset_t *assets,
                                 size_t n);

// Portable platform helpers. Implemented per-backend under
// platform/espidf/system/ and platform/arduino/system/.
//   bb_system_get_version: null-terminates out with the running firmware version.
//   bb_system_reboot: reboots the device; does not return on success.
void bb_system_get_version(char *out, size_t out_size);
void bb_system_reboot(void);

// Ensure the HTTP server is started (low-level helper; prefer bb_http_server_start).
// Used by provisioning and other advanced features. Idempotent.
bb_err_t bb_http_server_ensure_started(void);

#ifdef __cplusplus
}
#endif
