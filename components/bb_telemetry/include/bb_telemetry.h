// bb_telemetry — unified GET + PATCH /api/telemetry route.
//
// Section providers register via bb_telemetry_register_section; the route
// assembles GET responses as {<name>: {fields...}} and dispatches PATCH
// bodies by section name.
//
// Portable header — no esp_/cJSON/nvs_ includes.
#pragma once
#include "bb_core.h"
#include "bb_json.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Section callbacks.
// get_fn  writes fields into section (a new bb_json_t obj).
// patch_fn reads fields from section_patch (already-parsed sub-object).
//          NULL = read-only section; PATCH body with this key → 400.
typedef void     (*bb_telemetry_get_fn)  (bb_json_t section, void *ctx);
typedef bb_err_t (*bb_telemetry_patch_fn)(bb_json_t section_patch, void *ctx);

// Register a named telemetry section.
// Returns BB_ERR_NO_SPACE when registry full, BB_ERR_INVALID_ARG on null name/get.
bb_err_t bb_telemetry_register_section(const char *name,
                                        bb_telemetry_get_fn get,
                                        bb_telemetry_patch_fn patch,
                                        void *ctx);

// Like bb_telemetry_register_section but also accepts a JSON schema properties
// value for the section (used by bb_section_assemble_schema to build the real
// composed GET schema for /api/telemetry). Pass NULL for schema_props to skip.
bb_err_t bb_telemetry_register_section_ex(const char *name,
                                           bb_telemetry_get_fn get,
                                           bb_telemetry_patch_fn patch,
                                           void *ctx,
                                           const char *schema_props);

// Build GET response tree: per section, child=obj_new, get_fn(child),
// obj_set_obj(root, name, child).
void bb_telemetry_build_get(bb_json_t root);

// Dispatch PATCH body: per section present in body, call patch_fn(child).
// Returns BB_ERR_INVALID_ARG if a present section has patch_fn==NULL.
// Sets the pending-reboot flag when any section patch_fn returns BB_OK.
bb_err_t bb_telemetry_dispatch_patch(bb_json_t body);

// Returns true when a successful PATCH has been applied since boot or last
// reset.  Used by the route handler to signal {"reboot_required":true} and
// by GET /api/telemetry to include a "pending_reboot" field (B1-289).
bool bb_telemetry_pending_reboot(void);

// Assemble the real composed GET schema from all registered section schema_props.
// Returns a heap-allocated JSON schema string; caller must free.
// Intended for use in bb_telemetry_init to replace the generic {type:object}.
char *bb_telemetry_assemble_get_schema(void);

// Pure coupling helper: given post-patch sink state and whether publisher.enabled
// was explicitly set in the PATCH body, return the publisher enabled value to
// persist.  When publisher_explicit is true, publisher_explicit_value wins
// (user override).  Otherwise returns any_sink_enabled (auto-coupling).
// This function has no side effects and is host-testable.
bool bb_telemetry_couple_publisher(bool any_sink_enabled,
                                   bool publisher_explicit,
                                   bool publisher_explicit_value);

// Freeze the telemetry section registry: reject any registrations after this point.
// Called by bb_telemetry_init (order 5); also callable by tests.
void bb_telemetry_freeze(void);

// Register GET + PATCH /api/telemetry with the HTTP server.
// Called automatically when CONFIG_BB_TELEMETRY_AUTOREGISTER=y.
bb_err_t bb_telemetry_init(bb_http_handle_t server);

#ifdef BB_TELEMETRY_TESTING

// Reset registry for test isolation.
void bb_telemetry_reset_for_test(void);

// Test-hook aliases for build_get/dispatch_patch (same as the real fns).
void     bb_telemetry_build_get_for_test(bb_json_t root);
bb_err_t bb_telemetry_dispatch_patch_for_test(bb_json_t body);

#endif /* BB_TELEMETRY_TESTING */

#ifdef __cplusplus
}
#endif
