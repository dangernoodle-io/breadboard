#include "unity.h"
#include "bb_health.h"
#include "bb_health_test.h"
#include "bb_http_extender.h"
#include "bb_json.h"

#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#include "../../components/bb_health/bb_health_schema_priv.h"

static void test_health_extender_fn(void *root)
{
    (void)root;
}

// ---------------------------------------------------------------------------
// bb_health extender registration tests
// ---------------------------------------------------------------------------

void test_bb_health_register_extender_null_returns_err(void)
{
    bb_err_t err = bb_health_register_extender(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_health_register_extender_capacity(void)
{
    // Capacity is BB_HTTP_EXTENDER_MAX_PER_ROUTE (from the generic facility).
    // Fill the table completely.
    for (int i = 0; i < BB_HTTP_EXTENDER_MAX_PER_ROUTE; i++) {
        bb_err_t err = bb_health_register_extender(test_health_extender_fn);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
    }

    // One over capacity should return NO_SPACE.
    bb_err_t err = bb_health_register_extender(test_health_extender_fn);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

void test_bb_health_register_extender_ex_null_fn_returns_invalid_arg(void)
{
    bb_err_t err = bb_health_register_extender_ex(NULL, "\"x\":{\"type\":\"string\"}");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_health_register_after_freeze_returns_invalid_state(void)
{
    bb_health_freeze_for_test();
    bb_err_t err = bb_health_register_extender_ex(test_health_extender_fn, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

// ---------------------------------------------------------------------------
// Schema assembly tests
// ---------------------------------------------------------------------------

void test_bb_health_assembled_schema_no_extenders_equals_base_plus_suffix(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);

    char expected[4096];
    snprintf(expected, sizeof(expected), "%s%s",
             k_health_base, k_health_suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);
}

void test_bb_health_assembled_schema_is_valid_json(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled health schema is not valid JSON");
    cJSON_Delete(parsed);
}

void test_bb_health_assembled_schema_contains_fragment(void)
{
    static const char frag[] = "\"temp\":{\"type\":\"object\"}";
    bb_err_t err = bb_health_register_extender_ex(test_health_extender_fn, frag);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag),
                                 "fragment not found in assembled health schema");
}

void test_bb_health_assembled_schema_with_fragment_is_valid_json(void)
{
    static const char frag[] = "\"temp\":{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"}}}";
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_health_register_extender_ex(test_health_extender_fn, frag));
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "health schema with extender fragment is not valid JSON");
    cJSON_Delete(parsed);
}
