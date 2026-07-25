#pragma once

// bb_openapi_emit() — host-only tree emitter (B1-1116).
//
// Builds a complete OpenAPI 3.1 document from the bb_http route descriptor
// registry into a new bb_json_t tree. Zero device callers — the device path
// is bb_openapi_emit_stream() (components/bb_openapi/include/bb_openapi.h),
// which writes the document field-by-field via bb_http_resp_json_obj_* and
// never materializes a bb_json_t tree. This function IS the load-bearing
// entrypoint for host_tools/emit_openapi and the host test suites that
// assert against a fully-materialized document (test_openapi_emit.c,
// test_openapi_emit_equivalence.c, test_sse_schema_fidelity.c,
// test_route_fidelity.c), so it isn't deleted — relocated here instead of
// components/bb_openapi/include/bb_openapi.h so bb_json_t (and the cJSON
// backend it pulls in on ESP-IDF) never ships in firmware. Not part of the
// public bb_openapi API surface; not built by idf_component_register.
//
// See components/bb_openapi/include/bb_openapi.h for bb_openapi_meta_t and
// the operationId-derivation / schema-injection rules this emitter follows
// (identical to the device streaming path).

#include "bb_openapi.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns a bb_json_t tree that the caller must free with bb_json_free().
// Returns NULL on allocation failure or if meta is NULL.
bb_json_t bb_openapi_emit(const bb_openapi_meta_t *meta);

#ifdef __cplusplus
}
#endif
