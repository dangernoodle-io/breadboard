// bb_sensors — sectioned /api/sensors endpoint (fan/power/thermal).
//
// Owns a file-scope bb_response_registry_t; each breadboard section (fan,
// power, thermal) is registered at init. External consumers (e.g. TaipanMiner)
// may register additional sections via bb_sensors_register_section before the
// registry is frozen.
//
// Routes:
//   GET  /api/sensors  — bb_response_build_get over all registered sections.
//   PATCH /api/sensors — bb_response_dispatch_patch; fan section is PATCH-capable,
//                        power and thermal sections are read-only.
//
// The old /api/fan, /api/power, and /api/thermal routes coexist in this PR;
// they are deleted in PR7 (jae/bb-delete-old-routes).
//
// Host twin: platform/host/bb_sensors/bb_sensors_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"
#include "bb_response.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register a named section for /api/sensors.
//
// name         — section key in the GET /api/sensors response (e.g. "fan").
// get          — called at GET time; writes fields into the provided child bb_json_t.
// patch        — called at PATCH time for the matching key; NULL = read-only section.
// ctx          — opaque context pointer passed to get and patch.
// schema_props — complete JSON-Schema value for this section's object.
//                Must have static/rodata lifetime. NULL → no schema contribution.
//
// Returns BB_ERR_INVALID_ARG if name or get is NULL.
// Returns BB_ERR_INVALID_STATE if called after the registry is frozen.
// Returns BB_ERR_NO_SPACE if the section table is full.
bb_err_t bb_sensors_register_section(const char *name,
                                      bb_response_get_fn get,
                                      bb_response_patch_fn patch,
                                      void *ctx,
                                      const char *schema_props);

// Register GET+PATCH /api/sensors with the HTTP server (regular-tier init fn).
// Registers the built-in fan/power/thermal sections, freezes the registry,
// assembles the schema, then registers the routes.
bb_err_t bb_sensors_init(bb_http_handle_t server);

#ifdef BB_SENSORS_TESTING

// Test hook: freeze the registry (mirrors bb_health_freeze_for_test).
void bb_sensors_freeze_for_test(void);

// Test hook: invoke all registered section get_fns into root.
void bb_sensors_invoke_sections_for_test(bb_json_t root);

// Test hook: dispatch a PATCH body against registered sections.
bb_err_t bb_sensors_dispatch_patch_for_test(bb_json_t body);

// Test hook: reset state for test isolation.
void bb_sensors_reset_for_test(void);

// Test hook: return the assembled schema (cached, lazy).
const char *bb_sensors_get_assembled_schema(void);

// Test hook: call the real fan_section_patch directly (bypasses section dispatch).
// Allows testing fan PATCH field validation without an HTTP server or section registry.
// Only available when CONFIG_BB_FAN_AUTOFAN is set.
bb_err_t bb_sensors_fan_patch_for_test(bb_json_t patch_body);

#endif /* BB_SENSORS_TESTING */

#ifdef __cplusplus
}
#endif
