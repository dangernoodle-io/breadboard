#pragma once

// bb_http — shared HTTP vocabulary primitive.
//
// Holds ONLY the HTTP method enum and the route-DESCRIPTION types consumed by
// bb_openapi (and by every `*_routes` component's static route tables). This
// component carries no server implementation, no httpd dependency, and no
// state — see bb_http_server for the actual HTTP server (route registration,
// response/request helpers, JSON streaming, CORS, lifecycle). Consumers that
// only describe routes (bb_route_t tables) need just this component;
// consumers that also register/serve routes need bb_http_server too.

#include "bb_core.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
