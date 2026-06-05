#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// URL-decode a named field from a URL-encoded body (e.g., "field=value&...")
void bb_url_decode_field(const char *body, const char *field, char *out, size_t out_size);

// Strict boolean parser for form-urlencoded values. Returns true on success
// (with the parsed boolean in *out); false on empty/garbage input. Accepts
// case-insensitive: 1/true/on/yes/t/y -> true; 0/false/off/no/f/n -> false.
// Anything else (e.g. "maybe", "2", "") returns false without writing *out.
bool bb_url_parse_bool(const char *val, bool *out);

// Strict unsigned integer parser. Returns true on success (with the parsed
// value in *out); false on empty/garbage/overflow input. Accepts only
// decimal digits — no leading sign, no whitespace, no trailing junk.
bool bb_url_parse_uint(const char *val, unsigned long *out);

// ============================================================================
// PORTABLE HTTP SERVER API — available on all platforms (ESP-IDF, Arduino, host)
// ============================================================================

#include "bb_core.h"

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

// OpenAPI 3.1 ParameterObject descriptor. All pointer fields must remain valid
// for the life of the server (static/rodata).
typedef struct {
    const char *name;         // parameter name, e.g. "topic"
    const char *in;           // location: "query", "path", or "header"
    const char *description;  // human-readable one-liner; NULL to omit
    bool        required;     // true for path params; false for optional query/header params
    const char *schema_type;  // JSON Schema primitive type: "string", "integer", etc.
} bb_route_param_t;

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
    const bb_route_param_t   *parameters;            // NULL or pointer to parameters array
    size_t                    parameters_count;      // 0 when parameters is NULL
} bb_route_t;

// Register a described route. Equivalent to bb_http_register_route() plus
// adding the descriptor to a process-wide registry that bb_openapi walks.
// On registration failure the descriptor is NOT added to the registry.
bb_err_t bb_http_register_described_route(bb_http_handle_t server,
                                          const bb_route_t *route);

// Register a table of described routes in one call. Calls
// bb_http_register_described_route for each entry. Stops at the first
// failure and returns its error. Convenience wrapper for consumers that
// keep their imperative route descriptors in a static array (and pair
// with bb_http_reserve_routes(n) before server start to size the cap).
bb_err_t bb_http_register_route_table(bb_http_handle_t server,
                                      const bb_route_t * const *table,
                                      size_t n);

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

// Response helpers — usable inside a handler.
bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code);
bb_err_t bb_http_resp_set_type(bb_http_request_t *req, const char *mime);
bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value);

// Convenience send: sets Content-Type and sends str as a single chunk, then
// ends the chunked response. Equivalent to send_chunk(str) + send_chunk(NULL,0).
bb_err_t bb_http_resp_sendstr(bb_http_request_t *req, const char *str);

// Chunked send for streaming responses (e.g. SSE).
// len < 0 means compute length via strlen(buf).
// buf == NULL and len == 0 ends the chunked response.
bb_err_t bb_http_resp_send_chunk(bb_http_request_t *req, const char *buf, int len);

// Send a bodyless "204 No Content" response: sets the 204 status and finishes
// with no body and no chunked Transfer-Encoding (Content-Length: 0). Use for a
// successful POST/PUT/DELETE that returns nothing. Do NOT use
// send_chunk(NULL, 0) for this — it frames the 204 as chunked, which strict
// HTTP proxies (e.g. node http-proxy) reject as a protocol error.
bb_err_t bb_http_resp_no_content(bb_http_request_t *req);

// Forward declaration for JSON streaming (avoids circular dependency).
typedef void *bb_json_t;

// ============================================================================
// STREAMING JSON ARRAY API
// ============================================================================

// Single-use streaming JSON array container. After begin(), caller repeatedly
// calls emit() for each array element, then calls end() to close. The stream
// holds a sticky error: the first failed emit() poisons the stream (subsequent
// emits are no-ops), and end() returns that error. The _open guard prevents
// re-use after end().
//
// On ESP-IDF, this streams true chunked output: "[" in begin(), "," followed
// by each serialized item, "]" and stream termination in end(). Memory usage
// is bounded by one per-item JSON subtree at a time.
//
// On Arduino and host, items are buffered into a root array; end() serializes
// and sends the whole array as a single response. Same external behavior,
// buffered internally.
typedef struct bb_http_json_stream_s {
    void   *_req;       /* bb_http_request_t * */
    int     _err;       /* sticky first error, BB_OK initially */
    uint8_t _first;     /* nonzero until first element emitted */
    uint8_t _open;      /* nonzero between begin and end */
} bb_http_json_stream_t;

/* Begin a streaming JSON array response. Sets Content-Type: application/json,
 * opens chunked transfer-encoding, emits "[". After return the caller emits
 * elements via _emit, then closes via _end (always — even on error). */
bb_err_t bb_http_resp_json_arr_begin(bb_http_request_t *req,
                                     bb_http_json_stream_t *out);

/* Serialize one bb_json_t (object or scalar — typically a per-item object)
 * into the stream, preceded by "," for all but the first element. Caller
 * retains ownership of `item` and is responsible for bb_json_free(item).
 * Errors are sticky and surfaced by _end. */
bb_err_t bb_http_resp_json_arr_emit(bb_http_json_stream_t *stream,
                                    bb_json_t item);

/* Close the array: emits "]", ends the chunked response, returns the sticky
 * error (BB_OK if all emits succeeded). Always closes the underlying response
 * regardless of sticky-err state. Single-use. */
bb_err_t bb_http_resp_json_arr_end(bb_http_json_stream_t *stream);

// ============================================================================
// STREAMING JSON OBJECT API
// ============================================================================

// Single-use streaming JSON object emitter. Field-by-field emission with
// comma tracking and JSON escaping — no full-tree buffer ever needed.
//
// Usage pattern:
//   bb_http_json_obj_stream_t obj;
//   bb_http_resp_json_obj_begin(req, &obj);
//   bb_http_resp_json_obj_set_str(&obj, "name", "alice");
//   bb_http_resp_json_obj_set_int(&obj, "age", 30);
//   bb_http_resp_json_obj_end(&obj);
//
// Internal buffer: fields are accumulated in a ~1 KB stack buffer per stream;
// the buffer is flushed via bb_http_resp_send_chunk whenever it would overflow.
// _end() flushes the remainder and sends the chunked-transfer terminator.
//
// Nesting: _set_obj_begin/_set_obj_end and _set_arr_begin/_set_arr_end allow
// nested objects and arrays. Each nesting level increments _depth; the open
// guard and comma tracking apply per-context at emit time.
//
// Sticky error: the first failure poisons the stream; all subsequent calls are
// no-ops. _end() always flushes and closes the response, then returns the
// sticky error.
#define BB_HTTP_JSON_OBJ_BUF_SIZE 1024

typedef struct bb_http_json_obj_stream_s {
    void    *_req;                           /* bb_http_request_t * */
    int      _err;                           /* sticky first error, BB_OK initially */
    uint8_t  _open;                          /* nonzero between begin and end */
    uint8_t  _depth;                         /* nesting depth (0 = top-level obj) */
    uint8_t  _needs_comma[8];               /* per-depth comma flag (max 8 levels) */
    char     _buf[BB_HTTP_JSON_OBJ_BUF_SIZE]; /* internal flush buffer */
    size_t   _buf_len;                       /* bytes used in _buf */
} bb_http_json_obj_stream_t;

/* Begin a streaming JSON object response. Sets Content-Type: application/json,
 * opens chunked transfer-encoding, emits "{". After return the caller emits
 * fields via _set_*, then closes via _end (always — even on error). */
bb_err_t bb_http_resp_json_obj_begin(bb_http_request_t *req,
                                     bb_http_json_obj_stream_t *out);

/* Emit "key":"val" with JSON escaping on val.
 * val may be NULL; emits "key":null in that case. */
bb_err_t bb_http_resp_json_obj_set_str(bb_http_json_obj_stream_t *stream,
                                       const char *key, const char *val);

/* Emit "key":N as a floating-point number. */
bb_err_t bb_http_resp_json_obj_set_num(bb_http_json_obj_stream_t *stream,
                                       const char *key, double val);

/* Emit "key":N formatted as an integer (int64_t). */
bb_err_t bb_http_resp_json_obj_set_int(bb_http_json_obj_stream_t *stream,
                                       const char *key, int64_t val);

/* Emit "key":true or "key":false. */
bb_err_t bb_http_resp_json_obj_set_bool(bb_http_json_obj_stream_t *stream,
                                        const char *key, bool val);

/* Emit "key":null. */
bb_err_t bb_http_resp_json_obj_set_null(bb_http_json_obj_stream_t *stream,
                                        const char *key);

/* Begin a nested object: emit "key":{ and push depth.
 * Caller follows with _set_* calls, then calls _set_obj_end. */
bb_err_t bb_http_resp_json_obj_set_obj_begin(bb_http_json_obj_stream_t *stream,
                                             const char *key);

/* End a nested object: emit } and pop depth. */
bb_err_t bb_http_resp_json_obj_set_obj_end(bb_http_json_obj_stream_t *stream);

/* Begin a nested array: emit "key":[ and push depth.
 * Array elements should be emitted directly via _raw (see below) or via the
 * arr API after opening. When inside an array context, there is no key. */
bb_err_t bb_http_resp_json_obj_set_arr_begin(bb_http_json_obj_stream_t *stream,
                                             const char *key);

/* End a nested array: emit ] and pop depth. */
bb_err_t bb_http_resp_json_obj_set_arr_end(bb_http_json_obj_stream_t *stream);

/* Close the top-level object: flush buffer, emit "}", end chunked response,
 * return sticky error. Always closes regardless of sticky-err state. */
bb_err_t bb_http_resp_json_obj_end(bb_http_json_obj_stream_t *stream);

// Request accessors — MVP: read the body as a single buffer.
int bb_http_req_body_len(bb_http_request_t *req);
int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size);

// Return the underlying socket fd for the request (ESP-IDF only; used for SSE eviction).
int bb_http_req_sockfd(bb_http_request_t *req);

// Read a query-string value from the request URL.
// Returns BB_OK if found; BB_ERR_NOT_FOUND if key is absent.
bb_err_t bb_http_req_query_key_value(bb_http_request_t *req, const char *key,
                                     char *out, size_t out_len);

// Async handler support (ESP-IDF only; used for SSE long-lived connections).
bb_err_t bb_http_req_async_handler_begin(bb_http_request_t *req,
                                         bb_http_request_t **out_async_req);
bb_err_t bb_http_req_async_handler_complete(bb_http_request_t *async_req);

// Unregister a previously-registered route handler.
bb_err_t bb_http_unregister_route(bb_http_handle_t server,
                                  bb_http_method_t method,
                                  const char *path);

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

// Ensure the HTTP server is started (low-level helper; prefer bb_http_server_start).
// Used by provisioning and other advanced features. Idempotent.
bb_err_t bb_http_server_ensure_started(void);

// Handler count telemetry: observe the current number of registered handlers
// and the configured maximum (cap). Returns 0 on non-ESP-IDF platforms.
size_t bb_http_route_handler_count(void);
size_t bb_http_route_handler_cap(void);

// Reserve N additional handler slots beyond the auto-sized sum from the
// registry. Used by imperative-route consumers (e.g. bb_prov) that register
// routes outside the BB_REGISTRY_REGISTER_N path. Cumulative across calls.
// MUST be called before bb_http_server_ensure_started — once httpd_start
// has run, the cap is fixed.
void bb_http_reserve_routes(int n);

// Return true if `uri` matches any route in the registry (exact or wildcard-
// suffix match, same semantics as httpd_uri_match_wildcard). The catch-all
// wildcards registered by bb_http itself — OPTIONS /* and GET /* — are
// excluded from consideration so a bogus path under them still returns false.
// Portable: usable on host (test) and ESP-IDF platforms.
bool bb_http_uri_is_registered(const char *uri);

#ifdef __cplusplus
}
#endif
