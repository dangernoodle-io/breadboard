#pragma once

// bb_openapi_validate — host-only JSON Schema structural validator.
//
// B1-1151: this was a public bb_openapi API with zero production callers
// (nothing in firmware ever calls it) but it IS the load-bearing oracle for
// two host test suites (test_route_fidelity.c, test_sse_schema_fidelity.c)
// that assert HTTP/SSE response bodies match their declared OpenAPI schemas.
// Rather than delete it (which would gut that coverage) or ship it in
// firmware (which pulls all 31 cJSON call sites + the "json" component into
// the image for a function nothing running on-device ever calls), it moved
// to platform/host/ as a host-test-only helper: not part of the public
// bb_openapi API surface (components/bb_openapi/include/bb_openapi.h), not
// built by idf_component_register, only reachable via the
// bbtool-scaffold-hint in components/bb_openapi/CMakeLists.txt that adds it
// to host test builds.
//
// Minimal structural validator for the JSON Schema keywords used in
// breadboard's schema corpus: type, properties, required, items, enum,
// additionalProperties (false only). Unknown keywords are logged and ignored.

#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Validation error: path is a dotted JSON-pointer-ish string ("", "foo.bar",
// "arr[3].key"); message is a human-readable description of the failure.
typedef struct {
    char path[64];      // dotted path to the offending node
    char message[192];  // human-readable failure reason
} bb_openapi_validate_err_t;

// Validate `value` (already-parsed JSON tree, as a bb_json_t handle) against
// `schema_json` (raw JSON Schema string literal).
//
// Returns BB_OK if valid; err is untouched.
// Returns BB_ERR_VALIDATION if invalid; err (if non-NULL) is filled with
//   the first failure encountered: path + message.
// Returns BB_ERR_INVALID_ARG if schema_json fails to parse as JSON.
//
// Memory: schema_json is parsed internally and freed before return.
//   The caller retains ownership of value and schema_json.
bb_err_t bb_openapi_validate(const char *schema_json,
                             bb_json_t value,
                             bb_openapi_validate_err_t *err);

#ifdef __cplusplus
}
#endif
