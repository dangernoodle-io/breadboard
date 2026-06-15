#pragma once

#include "bb_nv.h"
#include "bb_section.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register a named info section.
//
// name         — section key in the GET /api/info response object (e.g. "display").
// get          — called at GET time; writes fields into the provided child bb_json_t.
// ctx          — opaque context pointer passed to get.
// schema_props — complete JSON-Schema value for this section's object
//                (e.g. '{"type":"object","properties":{...}}').
//                Must have static/rodata lifetime. NULL → no schema contribution.
//
// Returns BB_ERR_INVALID_ARG if name or get is NULL.
// Returns BB_ERR_INVALID_STATE if called after the registry is frozen.
// Returns BB_ERR_NO_SPACE if the section table is full.
bb_err_t bb_info_register_section(const char *name,
                                   bb_section_get_fn get,
                                   void *ctx,
                                   const char *schema_props);

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
// Must be called before bb_http_server_start (before the section table is
// frozen). If called after the server has started, the call is ignored and a
// warning is logged.
//
// If the registry is full (BB_INFO_MAX_CAPABILITIES reached) the extra
// registration is dropped and a warning is logged.
void bb_info_register_capability(const char *name);

#ifdef __cplusplus
}
#endif
