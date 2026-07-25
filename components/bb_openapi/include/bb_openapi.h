#pragma once

// Opt-in OpenAPI 3.1 spec emitter.
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
//   3. After bb_http_server_start(), call bb_openapi_init(server) (composition
//      root, e.g. codegen'd bb_app_init()) once to register the
//      GET /api/openapi.json route.
//   4. The runtime endpoint walks the route descriptor registry populated by
//      bb_http_register_described_route().

#include "bb_http.h"
#include "bb_http_server.h"

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

// Streaming variant: writes the OpenAPI document directly to the response via
// bb_http_resp_json_obj_* (bb_http_server's streaming JSON object primitive)
// so peak memory is bounded to that primitive's internal buffer plus one
// schema literal at a time, never a full in-memory JSON tree. Avoids the
// heap+stack pressure of materializing a full in-memory JSON document on a
// board with many routes — an earlier in-memory tree builder,
// bb_openapi_emit(), crashed httpd workers on tdongle-s3 / ESP32-WROOM-32
// once the route count grew past ~50 routes (B1-222); it was removed in
// B1-1054 once this streaming path reached full parity. This path never
// calls bb_json_*.
//
// Owns the full response lifecycle: opens the chunked response (sets
// Content-Type, emits "{") and, once opened, always closes it (flushes the
// remainder and sends the chunked terminator), even on error. Callers must
// NOT send their own terminator chunk. If opening the response itself fails
// (e.g. Content-Type can't be set), nothing was ever sent, so this returns
// that error directly without a close step — there is no open response for
// a terminator to belong to.
//
// Returns BB_OK on success or the first non-OK error encountered (sticky —
// once an internal write fails, later writes are no-ops, but the response is
// still finalized).
bb_err_t bb_openapi_emit_stream(bb_http_request_t *req,
                                const bb_openapi_meta_t *meta);

// Set metadata for OpenAPI spec emission. Must be called before bb_openapi's
// routes are registered / before the app hands off to the HTTP server —
// i.e. before the REGULAR-tier init that registers routes
// (bb_app_init_rest()/bb_app_init() under codegen, or the equivalent
// hand-wired call) — if custom metadata is desired.
// meta pointer must remain valid for the lifetime of the server.
// Calling bb_openapi_set_meta is optional; defaults are used otherwise.
void bb_openapi_set_meta(const bb_openapi_meta_t *meta);

// ---------------------------------------------------------------------------
// Schema component registry
// ---------------------------------------------------------------------------
// Maximum number of schema components that can be registered.
// 16 topics registered (14 new in B1-413 + 2 from B1-413-PR2); 24 provides headroom.
#define BB_OPENAPI_SCHEMA_REGISTRY_CAP 24

// Schema entry returned by bb_openapi_schema_get.
typedef struct {
    const char *component_name;  // key under components/schemas
    const char *schema_literal;  // JSON Schema literal (static storage)
    const char *sse_topic;       // bb_event topic name; NULL = REST-only
} bb_openapi_schema_entry_t;

// Register a named OpenAPI component schema.
// component_name: key under components/schemas (e.g. "LogEvent")
// schema_literal: complete JSON Schema literal (must be static storage; pointer stored)
// sse_topic:      bb_event topic this schema describes; NULL for REST-only
//
// Returns BB_OK on success.
// Returns BB_ERR_NO_SPACE if the registry is full (cap BB_OPENAPI_SCHEMA_REGISTRY_CAP).
// Returns BB_ERR_INVALID_ARG if component_name or schema_literal is NULL.
// Duplicate component_name: silently deduplicated (idempotent; first-wins).
bb_err_t bb_openapi_register_schema(const char *component_name,
                                    const char *schema_literal,
                                    const char *sse_topic);

// Convenience: bb_openapi_register_schema(component_name, schema_literal, topic_name).
bb_err_t bb_openapi_register_topic_schema(const char *topic_name,
                                          const char *schema_literal,
                                          const char *component_name);

// Clear the schema registry (test isolation).
void bb_openapi_schema_registry_clear(void);

// Number of registered schemas.
size_t bb_openapi_schema_count(void);

// Get a schema entry by index. Returns false if idx >= count or out is NULL.
bool bb_openapi_schema_get(size_t idx, bb_openapi_schema_entry_t *out);

// ---------------------------------------------------------------------------
// External topic-schema source seam (B1-1220 PR2)
// ---------------------------------------------------------------------------
// bb_openapi never links bb_data_http (or any other topic-schema producer
// component) directly -- breadboard ships primitives, composition is the
// consumer's job (see CLAUDE.md's composition-only banner). Instead, both the
// SSE oneOf synthesis (sse_schema_count() / build_sse_oneof_fragment()) AND
// the components/schemas section (bb_openapi_schemas_union_count() /
// emit_external_schemas(), bb_openapi_emit_shared.c) can be pointed at an
// EXTERNAL topic-schema table through this injected seam -- the exact same
// shape bb_data_http itself uses for its own render/generation-read/send
// seams (bb_data_http.h): an installable function pointer, NULL default,
// degrades gracefully rather than requiring a link-time dependency.

// Callback invoked once per external topic-schema entry by a
// bb_openapi_topic_source_fn_t. Shape matches bb_data_http_describe_cb_t
// (components/bb_data_http/include/bb_data_http.h) EXACTLY -- bb_openapi
// keeps its own copy of the typedef rather than including that header, so a
// composition root can pass bb_data_http_describe_foreach() directly as
// `fn` below with no adapter, no cast.
typedef void (*bb_openapi_topic_cb_t)(const char *key, const char *topic,
                                      const char *component_name,
                                      const char *schema_literal, void *ctx);

// A foreach-style walker over some external topic-schema table: calls `cb`
// once per entry, passing `ctx` through unchanged (the CALLER's own
// accumulator, supplied fresh per call -- see bb_openapi_emit_shared.c's
// use). Shape matches bb_data_http_describe_foreach() exactly, by design.
//
// Unlike bb_data_http's own render/generation-read/send seams (which wrap a
// call needing a BOUND opaque handle, hence their `void *ctx` stored
// alongside `fn` at wiring time), the function wired here is itself a
// foreach walker whose (cb, ctx) signature already carries per-call context
// supplied by bb_openapi at each emit -- there is no separate wiring-time
// state to bind, so bb_openapi_set_topic_source_fn() below takes no `ctx`
// parameter. A source needing its own bound state is expected to close over
// it as a file-scope static in its own TU, exactly like
// bb_data_http_describe_foreach() does over its describe table.
typedef void (*bb_openapi_topic_source_fn_t)(bb_openapi_topic_cb_t cb, void *ctx);

// Install/replace the external topic-schema source seam. `fn` is called
// alongside (after) the legacy schema registry above whenever
// sse_schema_count() / build_sse_oneof_fragment() OR bb_openapi_schemas_
// union_count() / emit_external_schemas() run, so its entries are unioned
// into both the SSE oneOf synthesis AND the components/schemas section with
// stable ordering (legacy first) -- a component reachable only through the
// seam always gets a body alongside its $ref, never a dangling reference.
// Dedup is by component_name, legacy-first-wins, across the WHOLE union --
// including entry-vs-entry WITHIN `fn`'s own walk: `fn` (e.g.
// bb_data_http_describe_foreach()) may dedup its own entries by a different
// key (bb_data_http_describe() dedups by `key`, not component_name), so two
// of its entries can legitimately share one component_name; this seam still
// emits that component_name exactly once (first-registered wins), never a
// duplicate JSON key or a duplicate oneOf branch.
// Passing fn=NULL (the default -- no bb_openapi_set_topic_source_fn() call
// at all is equivalent) disables this source entirely: documents synthesize
// from the legacy bb_openapi_register_topic_schema() registry alone, byte-
// identical to pre-B1-1220 behavior.
//
// Single-slot, last-write-wins: a second bb_openapi_set_topic_source_fn()
// call replaces the first outright, it does NOT chain both. A composition
// root that needs to union TWO external sources is responsible for writing
// its own small combinator TU (a `fn` that calls both real sources' foreach
// functions in turn) and installing that single combinator here -- bb_openapi
// does not provide multi-source fan-out itself.
//
// IMPORTANT: a composition root with ANY producer that calls
// bb_data_http_describe() (rather than the legacy
// bb_openapi_register_topic_schema()) MUST wire this seam --
// bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach) is the
// intended pairing -- or those producers' topic schemas silently never
// appear in /api/openapi.json. bb_openapi has no way to detect a populated-
// but-unwired describe table without a dependency on bb_data_http, which
// this seam exists specifically to avoid; examples/smoke now wires it (see
// its entry_espidf.c) -- see test_sse_schema_fidelity.c's
// test_sse_schema_union_seam_unwired_describe_entry_does_not_appear /
// test_sse_schema_union_seam_wired_describe_entry_appears_in_oneof pair for
// the unwired-vs-wired coverage.
void bb_openapi_set_topic_source_fn(bb_openapi_topic_source_fn_t fn);

// ---------------------------------------------------------------------------
// Registry hooks
// ---------------------------------------------------------------------------

// Reserve route-table slots for bb_openapi before the HTTP server starts.
// bbtool:init tier=pre_http fn=bb_openapi_reserve_routes
bb_err_t bb_openapi_reserve_routes(void);

// Registry hook — registers GET /api/openapi.json.
// bbtool:init tier=regular fn=bb_openapi_init server=true
bb_err_t bb_openapi_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
