#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "bb_core.h"

#ifdef ESP_PLATFORM
#include "bb_json.h"

typedef void (*bb_health_extender_fn)(bb_json_t root);
#else
// Host stub: opaque handle for extender callbacks
typedef void (*bb_health_extender_fn)(void *root);
#endif

// Compute the /api/health.ok gate: true when WiFi has IP AND OTA is validated.
// mDNS is intentionally excluded (locked decision B1-269).
bool bb_health_compute_ok(void);

// Register an extender for /api/health. Fixed capacity (per-route slot count).
// Must be called before bb_http_server_start; registering after start returns
// BB_ERR_INVALID_STATE.
// Returns BB_ERR_NO_SPACE if the table is full.
// Thin wrapper around bb_health_register_extender_ex(fn, NULL).
bb_err_t bb_health_register_extender(bb_health_extender_fn fn);

// Register an extender for /api/health with an optional JSON-Schema properties
// fragment.
//
// schema_props_fragment: a comma-free, brace-free JSON properties fragment
//   that will be merged into the /api/health 200 response schema's "properties"
//   object. Example:
//     "\"temp\":{\"type\":\"object\"}"
//   Rules:
//     - No leading or trailing comma (the assembler adds commas between entries).
//     - No enclosing braces — these are entries inside "properties":{...}.
//     - Must have static/rodata lifetime (pointer is stored; string is not copied).
//     - NULL or empty string: no schema contribution (behaves like plain register).
//
// Returns BB_ERR_INVALID_ARG if fn is NULL.
// Returns BB_ERR_INVALID_STATE if called after the table is frozen (server started).
// Returns BB_ERR_NO_SPACE if the table is full.
bb_err_t bb_health_register_extender_ex(bb_health_extender_fn fn,
                                         const char *schema_props_fragment);

#ifdef __cplusplus
}
#endif
