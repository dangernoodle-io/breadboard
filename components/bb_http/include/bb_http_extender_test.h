#pragma once

// Test-only accessors for bb_http_extender — included only when
// BB_HTTP_TESTING is defined. Do NOT include from production code.

#ifdef BB_HTTP_TESTING

#ifdef __cplusplus
extern "C" {
#endif

// Reset all extender state: clears route table, frees assembled schemas,
// unfreezes. Called from setUp() so each test starts clean.
void bb_http_extender_reset_for_test(void);

// Return the assembled schema for route_id previously built by
// bb_http_route_assemble_schema(). NULL if not yet assembled or malloc failed.
// Caller must NOT free the result.
const char *bb_http_extender_get_assembled_schema(const char *route_id);

#ifdef __cplusplus
}
#endif

#endif /* BB_HTTP_TESTING */
