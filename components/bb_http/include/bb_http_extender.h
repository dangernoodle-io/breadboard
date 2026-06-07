#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Route-keyed extender registry for bb_http.
//
// Any component can register runtime extender callbacks and/or JSON-Schema
// properties fragments against a short string route_id (e.g. "info", "health").
// At init time, call bb_http_route_assemble_schema() to build the full schema
// string and bb_http_route_run_extenders() to invoke callbacks per-request.
//
// Caps:
//   BB_HTTP_EXTENDER_MAX_ROUTES  — number of distinct route_ids (default 8)
//   BB_HTTP_EXTENDER_MAX_PER_ROUTE — extenders per route_id (default 8)
//
// Lifetime rules:
//   - route_id, schema_props_fragment: must have static/rodata lifetime
//   - Registration must occur before the registry is frozen (server start)
//   - Assembled schema buffer is malloc'd; stable pointer returned; do NOT free

#define BB_HTTP_EXTENDER_MAX_ROUTES    8
#define BB_HTTP_EXTENDER_MAX_PER_ROUTE 8

// Extender callback type. On ESP-IDF, bb_json_t is the cJSON* handle;
// on host it is void* (same split as bb_info_extender_fn).
#ifdef ESP_PLATFORM
#include "bb_json.h"
typedef void (*bb_http_extender_fn)(bb_json_t root);
#else
typedef void (*bb_http_extender_fn)(void *root);
#endif

// Register an extender (and optional schema fragment) for a route_id.
//
// route_id:              short static string key (e.g. "info", "health")
// fn:                    callback invoked per-request via run_extenders
// schema_props_fragment: comma-free/brace-free JSON properties fragment;
//                        NULL or "" for runtime-only extender (no schema)
//
// Returns BB_ERR_INVALID_ARG  if fn is NULL.
// Returns BB_ERR_INVALID_STATE if called after freeze.
// Returns BB_ERR_NO_SPACE     if route or per-route capacity exceeded.
bb_err_t bb_http_register_route_extender(const char *route_id,
                                          bb_http_extender_fn fn,
                                          const char *schema_props_fragment);

// Invoke all extenders registered under route_id in registration order.
// root is passed directly to each fn. No-op if route_id has no extenders.
#ifdef ESP_PLATFORM
void bb_http_route_run_extenders(const char *route_id, bb_json_t root);
#else
void bb_http_route_run_extenders(const char *route_id, void *root);
#endif

// Assemble the schema for route_id:  base + ("," + frag)* + suffix
// Mallocs a stable buffer; returns a pointer that remains valid until the
// next bb_http_extender_reset_for_test() (test builds) or program end.
// Returns NULL on malloc failure.
// Fragments are the schema_props_fragment values registered for route_id;
// fragments that are NULL/empty are skipped.
const char *bb_http_route_assemble_schema(const char *route_id,
                                           const char *base,
                                           const char *suffix);

// Freeze the registry: all subsequent registration attempts return
// BB_ERR_INVALID_STATE. Called once (idempotent) at server-start time.
// On espidf this is called inside bb_info_init() / equivalent init fns.
// On host the test harness also calls this function.
void bb_http_extender_freeze(void);

#ifdef __cplusplus
}
#endif
