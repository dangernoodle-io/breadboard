#pragma once

// test_route_schema_walk_common -- shared registry-walk idiom: parse every
// non-NULL route->request_schema / route->responses[].schema string with
// cJSON_Parse and record a failure count + first failure message. Extracted
// (B1-1211 review) because test_route_schemas.c's
// check_schema_string()/route_schema_walker() and
// test_route_schema_literals_live.c's check_one()/live_schema_walker() were a
// near-verbatim second hand-roll of the same shape (past the project's
// second-instance extraction threshold).

#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

// Accumulator for a schema-literal walk over one or more routes.
typedef struct {
    int  route_count;         // routes visited via test_route_schema_walk()
    int  schema_count;        // non-NULL schema strings parsed
    int  failures;            // parse failures
    char first_failure[256];  // diagnostic for the first failure, if any
} test_route_schema_walk_ctx_t;

// Parse one schema string (if non-NULL) and record the result into ctx:
// increments schema_count, and on cJSON_Parse failure increments failures
// and (for the first failure only) formats a diagnostic -- including the
// parser's error offset -- into ctx->first_failure. route/which are used
// only for that diagnostic message. No-op if schema is NULL.
void test_route_schema_walk_check_one(const char *route, const char *which,
                                       const char *schema,
                                       test_route_schema_walk_ctx_t *ctx);

// bb_http_route_registry_foreach() callback: increments route_count, then
// runs test_route_schema_walk_check_one() over route->request_schema and
// every route->responses[].schema. No-op if route is NULL. Callers that need
// additional per-route checks (e.g. test_route_schemas.c's mutating-bare-body
// guard) call test_route_schema_walk_check_one() directly from their own
// walker instead of this whole-route convenience wrapper.
void test_route_schema_walk(const bb_route_t *route, void *ctx_);

#ifdef __cplusplus
}
#endif
