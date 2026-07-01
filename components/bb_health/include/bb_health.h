#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "bb_core.h"
#include "bb_response.h"

// Compute the /api/health.ok gate: true when WiFi has IP AND OTA is validated.
// mDNS is intentionally excluded (locked decision B1-269).
bool bb_health_compute_ok(void);

// Register a named section for /api/health.
//
// name         — section key in the GET /api/health response object (e.g. "mqtt").
// get          — called at GET time; writes fields into the provided child bb_json_t.
// ctx          — opaque context pointer passed to get.
// schema_props — complete JSON-Schema value for this section's object
//                (e.g. '{"type":"object","properties":{...}}').
//                Must have static/rodata lifetime. NULL → no schema contribution.
//
// Returns BB_ERR_INVALID_ARG if name or get is NULL.
// Returns BB_ERR_INVALID_STATE if called after the registry is frozen (server started).
// Returns BB_ERR_NO_SPACE if the section table is full.
bb_err_t bb_health_register_section(const char *name,
                                     bb_response_get_fn get,
                                     void *ctx,
                                     const char *schema_props);

#ifdef __cplusplus
}
#endif
