#pragma once

// Opt-in OpenAPI 3.1 spec emitter with auto-registration.
//
// Consumer firmware links this component only by adding `bb_openapi` to a
// REQUIRES/PRIV_REQUIRES line in one of its own components' idf_component_register
// calls. Without that explicit dependency, the component is discovered (via
// EXTRA_COMPONENT_DIRS) but not built or flashed.
//
// Typical use:
//   1. Add `bb_openapi` to REQUIRES in your app's main component.
//   2. Optionally call bb_openapi_set_meta(&meta) before bb_http_server_start();
//      if not called, defaults are used (title "breadboard device", version from
//      bb_system_get_version(), description and server_url NULL).
//   3. After bb_http_server_start(), call bb_registry_init() once to
//      auto-register the GET /api/openapi.json route.
//   4. The runtime endpoint walks the route descriptor registry populated by
//      bb_http_register_described_route().

#include "bb_http.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Metadata for the generated OpenAPI 3.1 document.
// All pointer fields must remain valid for the duration of the emit call (or
// the server lifetime when passed to bb_openapi_register).
typedef struct {
    const char *title;        // e.g. "TaipanMiner API"
    const char *version;      // semver string; consumer fills from app version
    const char *description;  // optional; one-paragraph summary; NULL omits the field
    const char *server_url;   // optional; e.g. "http://miner.local"; NULL omits "servers"
} bb_openapi_meta_t;

// Build a complete OpenAPI 3.1 document from the bb_http route descriptor
// registry into a new JSON tree rooted at the returned handle.
// Returns a bb_json_t tree that the caller must free with bb_json_free().
// Returns NULL on allocation failure.
//
// The emitter walks bb_http_route_registry_foreach and writes:
//   "openapi", "info", "servers" (if meta->server_url), "paths"
//
// operationId derivation (when route->operation_id is NULL):
//   <method><PathCamelCase> where:
//   - method is lowercased (get/post/...)
//   - path segments have leading '/' stripped and first letter uppercased
//   - '-' and '_' trigger next-char uppercasing
//   e.g. GET /api/stats -> "getApiStats"
//       POST /api/pool-config -> "postApiPoolConfig"
//
// Schema injection: route->request_schema and bb_route_response_t.schema are
// JSON Schema fragments (const char * JSON literals). They are injected as raw
// JSON objects via bb_json_obj_set_raw — NOT as strings.
//
// Path grouping: multiple methods on the same path are grouped under one path
// key. Up to 64 unique paths are supported (matches registry cap).
bb_json_t bb_openapi_emit(const bb_openapi_meta_t *meta);

// Set metadata for OpenAPI spec emission. Must be called before
// bb_registry_init() if custom metadata is desired.
// meta pointer must remain valid for the lifetime of the server.
// Calling bb_openapi_set_meta is optional; defaults are used otherwise.
void bb_openapi_set_meta(const bb_openapi_meta_t *meta);

#ifdef __cplusplus
}
#endif
