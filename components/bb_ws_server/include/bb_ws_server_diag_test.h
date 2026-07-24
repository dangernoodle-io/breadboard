#pragma once

// Test-only accessors for bb_ws_server_diag's runtime-composed OpenAPI
// schema (CONFIG_BB_OPENAPI_RUNTIME_META, B1-1059 PR-3 batch 1). Included
// only when BB_WS_SERVER_DIAG_TESTING is defined. Do NOT include from
// production code.

#ifdef BB_WS_SERVER_DIAG_TESTING

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs the same guarded, idempotent compose-and-patch bb_ws_server_diag_
// register() runs before registering (CONFIG_BB_OPENAPI_RUNTIME_META build
// only): assembles the schema via bb_serialize_meta_openapi_schema() into
// the file-scope buffer and patches
// s_ws_server_diag_describe_responses[0].schema the first time it's
// called; a no-op (returns BB_OK without re-assembling) on any subsequent
// call once the schema is already patched in. Returns whatever
// bb_serialize_meta_openapi_schema() returns.
bb_err_t bb_ws_server_diag_assemble_schema_for_test(void);

// Returns the describe route's current 200-response schema pointer (NULL
// before bb_ws_server_diag_assemble_schema_for_test() has run).
const char *bb_ws_server_diag_get_describe_schema_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_WS_SERVER_DIAG_TESTING */
