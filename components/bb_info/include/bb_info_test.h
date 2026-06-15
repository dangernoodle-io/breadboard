#pragma once

// Test-only accessors for bb_info — included only when BB_INFO_TESTING is defined.
// Do NOT include from production code.

#ifdef BB_INFO_TESTING

#ifdef __cplusplus
extern "C" {
#endif

// Returns the assembled /api/info 200 response schema, lazily constructed on
// first call from k_info_schema_base + registered section schemas +
// k_info_schema_suffix via bb_section_assemble_schema. NULL if malloc fails.
// Caller must NOT free the result.
const char *bb_info_get_assembled_schema(void);

// Freeze the section table so that registers after this point return
// BB_ERR_INVALID_STATE. Mirrors what bb_info_init() does on ESP-IDF.
// Safe to call multiple times (idempotent).
void bb_info_freeze_for_test(void);

// Reset all bb_info state: clears section tables, capabilities, unfreeze,
// free assembled schema. Called from setUp() in test_main.c to isolate tests.
void bb_info_reset_for_test(void);

// Invoke all registered /api/info section get_fns against root.
// Mirrors what bb_info's info_handler does on ESP-IDF so host tests can
// verify section JSON output without a live HTTP server.
void bb_info_invoke_sections_for_test(bb_json_t root);

#ifdef __cplusplus
}
#endif

#endif /* BB_INFO_TESTING */
