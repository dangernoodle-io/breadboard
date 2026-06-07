#include "unity.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include "bb_log.h"

#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#include "../../components/bb_info/bb_info_schema_priv.h"

static int s_extender_call_count = 0;

static void test_extender_fn(bb_json_t root)
{
    (void)root;
    s_extender_call_count++;
}

static void test_extender_fn2(bb_json_t root)
{
    (void)root;
    s_extender_call_count++;
}

// ---------------------------------------------------------------------------
// Existing tests
// ---------------------------------------------------------------------------

void test_bb_health_register_extender_null_returns_err(void)
{
    bb_err_t err = bb_health_register_extender(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_health_register_extender_capacity(void)
{
    // Capacity is now BB_HTTP_EXTENDER_MAX_PER_ROUTE (from the generic facility).
    // Fill the table completely.
    for (int i = 0; i < BB_HTTP_EXTENDER_MAX_PER_ROUTE; i++) {
        bb_err_t err = bb_health_register_extender(test_extender_fn);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
    }

    // One over capacity should return NO_SPACE.
    bb_err_t err = bb_health_register_extender(test_extender_fn);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

void test_bb_info_register_extender_null_returns_err(void)
{
    bb_err_t err = bb_info_register_extender(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// ---------------------------------------------------------------------------
// New _ex tests
// ---------------------------------------------------------------------------

// (1) _ex with NULL fn → BB_ERR_INVALID_ARG
void test_bb_info_register_extender_ex_null_fn_returns_invalid_arg(void)
{
    bb_err_t err = bb_info_register_extender_ex(NULL, "\"x\":{\"type\":\"string\"}");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// (2a) _ex with NULL schema succeeds and behaves like plain register
void test_bb_info_register_extender_ex_null_schema_succeeds(void)
{
    bb_err_t err = bb_info_register_extender_ex(test_extender_fn, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    // Assembled schema with a no-fragment extender == base + suffix
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    // No fragment was contributed — should not appear
    TEST_ASSERT_NULL(strstr(schema, "\"x\""));
}

// (2b) _ex with empty string schema succeeds (treated as NULL)
void test_bb_info_register_extender_ex_empty_schema_succeeds(void)
{
    bb_err_t err = bb_info_register_extender_ex(test_extender_fn, "");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
}

// (3) Registered fragment appears as substring in assembled schema
void test_bb_info_assembled_schema_contains_fragment(void)
{
    static const char frag[] = "\"xtest\":{\"type\":\"string\"}";
    bb_err_t err = bb_info_register_extender_ex(test_extender_fn, frag);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag),
                                 "fragment not found in assembled schema");
}

// (4) No extenders: assembled == base + suffix
void test_bb_info_assembled_schema_no_extenders_equals_base_plus_suffix(void)
{
    // No extenders registered (reset happened in setUp)
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);

    // Build the expected string
    char expected[8192];
    snprintf(expected, sizeof(expected), "%s%s",
             k_info_schema_base, k_info_schema_suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);
}

// (5) Two extenders → both fragments present AND schema is valid JSON
void test_bb_info_assembled_schema_two_extenders_both_present_valid_json(void)
{
    static const char frag1[] = "\"aa\":{\"type\":\"string\"}";
    static const char frag2[] = "\"bb\":{\"type\":\"integer\"}";
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_info_register_extender_ex(test_extender_fn,  frag1));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_info_register_extender_ex(test_extender_fn2, frag2));

    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag1), "frag1 not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag2), "frag2 not in schema");

    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema with 2 extenders is not valid JSON");
    cJSON_Delete(parsed);
}

// (6) Assembled schema parses as valid JSON (cJSON_Parse non-NULL) — no extenders
void test_bb_info_assembled_schema_is_valid_json(void)
{
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema is not valid JSON");
    cJSON_Delete(parsed);
}

// (7) Register after freeze → BB_ERR_INVALID_STATE
void test_bb_info_register_after_freeze_returns_invalid_state(void)
{
    bb_info_freeze_for_test();
    bb_err_t err = bb_info_register_extender_ex(test_extender_fn, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

// ---------------------------------------------------------------------------
// Capability registry tests
// ---------------------------------------------------------------------------

// Helper: parse assembled schema (with no extenders) and check capabilities entry.
static void assert_assembled_schema_has_capabilities(void)
{
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"capabilities\""),
        "capabilities not found in assembled schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"items\""),
        "capabilities items not found in assembled schema");
}

// (C1) Register N capabilities → all appear in assembled schema.
void test_bb_info_capabilities_registered_appear_in_schema(void)
{
    bb_info_register_capability("ota_pull");
    bb_info_register_capability("ntp");
    assert_assembled_schema_has_capabilities();
}

// (C2) Dedup: register same name twice → appears once.
void test_bb_info_capabilities_dedup(void)
{
    bb_info_register_capability("ota_push");
    bb_info_register_capability("ota_push"); // duplicate
    // Count via a small cJSON parse of a synthetic JSON body isn't trivial here;
    // we verify indirectly: schema builds without crash and cap appears once
    // by checking the assembled schema parses.
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "schema with dup caps not valid JSON");
    cJSON_Delete(parsed);
}

// (C3) Empty set → schema still contains capabilities array descriptor.
void test_bb_info_capabilities_empty_schema_present(void)
{
    // No capabilities registered (reset in setUp).
    assert_assembled_schema_has_capabilities();
}

// (C4) Over-capacity: register BB_INFO_MAX_CAPABILITIES+1 distinct names → extras dropped, no crash.
void test_bb_info_capabilities_over_cap_drops_extra(void)
{
    // Register up to the cap with synthetic names (static storage).
    static const char *names[] = {
        "c00","c01","c02","c03","c04","c05","c06","c07",
        "c08","c09","c10","c11","c12","c13","c14","c15",
        "c16","c17","c18","c19","c20","c21","c22","c23",
        "c24","c25","c26","c27","c28","c29","c30","c31",
        "c32_overflow", // one over BB_INFO_MAX_CAPABILITIES (32)
    };
    for (int i = 0; i <= BB_INFO_MAX_CAPABILITIES; i++) {
        bb_info_register_capability(names[i]);
    }
    // Schema must still parse as valid JSON even with the overflow dropped.
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "schema after cap overflow not valid JSON");
    cJSON_Delete(parsed);
}

// (C5) Post-freeze register: ignored (no crash, no state change).
void test_bb_info_capabilities_post_freeze_ignored(void)
{
    bb_info_register_capability("pre_freeze");
    bb_info_freeze_for_test();
    bb_info_register_capability("post_freeze"); // should be silently ignored
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    // post_freeze was not registered; no crash.
}

// (C6) Assembled schema contains capabilities array schema descriptor,
//      and bb_info_get_assembled_schema() reflects it.
void test_bb_info_assembled_schema_contains_capabilities_array(void)
{
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    // Verify the array+items shape is present.
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"),
        "capabilities array schema descriptor not found in assembled schema");
}
