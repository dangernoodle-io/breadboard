#include "unity.h"
#include "bb_info.h"
#include "bb_info_test.h"
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
    // Register 4 extenders (capacity is 4)
    for (int i = 0; i < 4; i++) {
        bb_err_t err = bb_health_register_extender(test_extender_fn);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
    }

    // 5th should return NO_SPACE
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
