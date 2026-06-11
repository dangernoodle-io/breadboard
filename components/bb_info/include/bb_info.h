#pragma once

#include "bb_nv.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "bb_json.h"

typedef void (*bb_info_extender_fn)(bb_json_t root);
#else
// Host stub: opaque handle for extender callbacks
typedef void (*bb_info_extender_fn)(void *root);
#endif

// Register an extender. Fixed capacity (4 slots). Must be called before
// bb_http_server_start; registering after start returns BB_ERR_INVALID_STATE.
// Returns BB_ERR_NO_SPACE if the table is full.
// Thin wrapper around bb_info_register_extender_ex(fn, NULL).
bb_err_t bb_info_register_extender(bb_info_extender_fn fn);

// Register an extender with an optional JSON-Schema properties fragment.
//
// schema_props_fragment: a comma-free, brace-free JSON properties fragment
//   that will be merged into the /api/info 200 response schema's "properties"
//   object. Example:
//     "\"display\":{\"type\":\"object\"},\"led\":{\"type\":\"string\"}"
//   Rules:
//     - No leading or trailing comma (the assembler adds commas between entries).
//     - No enclosing braces — these are entries inside "properties":{...}.
//     - Must have static/rodata lifetime (pointer is stored; string is not copied).
//     - NULL or empty string: no schema contribution (behaves like plain register).
//
// Returns BB_ERR_INVALID_ARG if fn is NULL.
// Returns BB_ERR_INVALID_STATE if called after the table is frozen (server started).
// Returns BB_ERR_NO_SPACE if the table is full.
bb_err_t bb_info_register_extender_ex(bb_info_extender_fn fn,
                                       const char *schema_props_fragment);

// ---------------------------------------------------------------------------
// Capability registry
// ---------------------------------------------------------------------------

// Maximum number of distinct capabilities that can be registered.
#define BB_INFO_MAX_CAPABILITIES 32

// Register a capability flag by name.
//
// name must have static/rodata lifetime — the pointer is stored, the string
// is NOT copied. Presence-only: register only capabilities that are present.
// Duplicates are silently ignored (string compare).
//
// Must be called before bb_http_server_start (before the extender table is
// frozen). If called after the server has started, the call is ignored and a
// warning is logged — mirrors the extender post-freeze behaviour.
//
// If the registry is full (BB_INFO_MAX_CAPABILITIES reached) the extra
// registration is dropped and a warning is logged.
void bb_info_register_capability(const char *name);

#ifdef __cplusplus
}
#endif
