#pragma once

// Test-only accessors for bb_sensor_http_wire's runtime-composed OpenAPI
// schemas (CONFIG_BB_OPENAPI_RUNTIME_META, B1-1059 PR-3 batch 3). Included
// only when BB_SENSOR_HTTP_WIRE_TESTING is defined. Do NOT include from
// production code.
//
// Four independently-composed buffers: GET /api/sensors/fan response, PATCH
// /api/sensors/fan request, GET /api/sensors/power response, and GET
// /api/sensors/thermal response.

#ifdef BB_SENSOR_HTTP_WIRE_TESTING

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Each pair below runs the same guarded, idempotent compose-and-patch step
// bb_sensor_http_describe_routes() runs before registering
// (CONFIG_BB_OPENAPI_RUNTIME_META build only): assembles the schema via
// bb_serialize_meta_openapi_schema() into its file-scope buffer and patches
// the corresponding describe-route field the first time it's called; a
// no-op (returns BB_OK without re-assembling) on any subsequent call once
// already patched. Returns whatever bb_serialize_meta_openapi_schema()
// returns.
bb_err_t bb_sensor_http_wire_assemble_fan_schema_for_test(void);
bb_err_t bb_sensor_http_wire_assemble_fan_request_schema_for_test(void);
bb_err_t bb_sensor_http_wire_assemble_power_schema_for_test(void);
bb_err_t bb_sensor_http_wire_assemble_thermal_schema_for_test(void);

// Returns the corresponding describe route's current schema/request_schema
// pointer (NULL before its assemble accessor above has run).
const char *bb_sensor_http_wire_get_fan_describe_schema_for_test(void);
const char *bb_sensor_http_wire_get_fan_describe_request_schema_for_test(void);
const char *bb_sensor_http_wire_get_power_describe_schema_for_test(void);
const char *bb_sensor_http_wire_get_thermal_describe_schema_for_test(void);

// Registers the real production PATCH /api/sensors/fan describe route
// (bb_http_register_route_descriptor_only) so a test can drive
// bb_openapi_emit_stream() over it directly.
bb_err_t bb_sensor_http_wire_register_fan_patch_route_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_SENSOR_HTTP_WIRE_TESTING */
