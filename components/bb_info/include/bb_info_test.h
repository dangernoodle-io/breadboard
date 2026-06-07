#pragma once

// Test-only accessors for bb_info — included only when BB_INFO_TESTING is defined.
// Do NOT include from production code.

#ifdef BB_INFO_TESTING

#ifdef __cplusplus
extern "C" {
#endif

// Returns the assembled /api/info 200 response schema, lazily constructed on
// first call from k_info_schema_base + registered extender fragments +
// k_info_schema_suffix. NULL if malloc fails. Caller must NOT free the result.
const char *bb_info_get_assembled_schema(void);

// Freeze the extender table so that registers after this point return
// BB_ERR_INVALID_STATE. Mirrors what bb_info_init() does on ESP-IDF.
// Safe to call multiple times (idempotent).
void bb_info_freeze_for_test(void);

// Reset all bb_info state: clears extender tables, unfreeze, free assembled schema.
// Called from setUp() in test_main.c to isolate tests.
void bb_info_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_INFO_TESTING */
