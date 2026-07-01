#pragma once

// Test-only accessors for bb_health — included only when BB_HEALTH_TESTING is defined.
// Do NOT include from production code.

#ifdef BB_HEALTH_TESTING

#ifdef __cplusplus
extern "C" {
#endif

#include "bb_json.h"

// Freeze the section table so that registers after this point return
// BB_ERR_INVALID_STATE. Mirrors what bb_health_init() does on ESP-IDF.
// Safe to call multiple times (idempotent).
void bb_health_freeze_for_test(void);

// Reset all bb_health state: clears section table, unfreeze, free assembled schema.
// Called from setUp() in test_main.c to isolate tests.
void bb_health_reset_for_test(void);

// Invoke all registered /api/health section get_fns against root.
// Mirrors what bb_health's health_handler does on ESP-IDF so host tests can
// verify section JSON output without a live HTTP server.
void bb_health_invoke_sections_for_test(bb_json_t root);

// Returns the assembled /api/health 200 response schema, lazily constructed on
// first call from k_health_base + registered section schemas +
// k_health_suffix via bb_response_assemble_schema. NULL if malloc fails.
// Caller must NOT free the result.
const char *bb_health_get_assembled_schema(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_HEALTH_TESTING */
