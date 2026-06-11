#include "unity.h"
#include "bb_temp.h"
#include "bb_health.h"
#include "bb_health_test.h"
#include "bb_json.h"

#include "../../platform/host/bb_temp/bb_temp_test.h"

#include <string.h>
#include <stdio.h>

/* setUp/tearDown are called per-test by the test runner via test_main.c setUp(). */

/* ---- bb_temp_read_soc ---- */

void test_bb_temp_read_soc_default_absent(void)
{
    /* Default host state: present=false → read returns false */
    bb_temp_test_set_soc(false, 0.0f);
    float c = -999.0f;
    bool ok = bb_temp_read_soc(&c);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_FLOAT(-999.0f, c); /* *out untouched on false */
}

void test_bb_temp_read_soc_injected_present(void)
{
    bb_temp_test_set_soc(true, 42.5f);
    float c = 0.0f;
    bool ok = bb_temp_read_soc(&c);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_FLOAT(42.5f, c);
}

void test_bb_temp_read_soc_null_out_returns_false(void)
{
    bb_temp_test_set_soc(true, 30.0f);
    bool ok = bb_temp_read_soc(NULL);
    TEST_ASSERT_FALSE(ok);
}

/* ---- bb_temp_register_info health extender ---- */

void test_bb_temp_health_extender_absent_by_default(void)
{
    /* Default: present=false → extender emits temp:{present:false} */
    bb_temp_test_set_soc(false, 0.0f);
    bb_temp_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_health_invoke_extenders_for_test(root);

    bb_json_t temp = bb_json_obj_get_item(root, "temp");
    TEST_ASSERT_NOT_NULL_MESSAGE(temp, "temp key missing from health extender output");

    bool present = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(temp, "present", &present));
    TEST_ASSERT_FALSE(present);

    /* soc_c should not be present */
    double c = -999.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(temp, "soc_c", &c),
                              "soc_c should be absent when present=false");

    bb_json_free(root);
}

void test_bb_temp_health_extender_present_with_value(void)
{
    bb_temp_test_set_soc(true, 55.3f);
    bb_temp_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_health_invoke_extenders_for_test(root);

    bb_json_t temp = bb_json_obj_get_item(root, "temp");
    TEST_ASSERT_NOT_NULL_MESSAGE(temp, "temp key missing from health extender output");

    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(temp, "present", &present));
    TEST_ASSERT_TRUE(present);

    double soc_c = 0.0;
    TEST_ASSERT_TRUE_MESSAGE(bb_json_obj_get_number(temp, "soc_c", &soc_c),
                             "soc_c should be present when present=true");
    /* 55.3 rounded to 1 decimal → 55.3 */
    TEST_ASSERT_DOUBLE_WITHIN(0.05, 55.3, soc_c);

    bb_json_free(root);
}

void test_bb_temp_health_extender_value_rounds_to_one_decimal(void)
{
    /* 36.789 → rounds to 36.8 */
    bb_temp_test_set_soc(true, 36.789f);
    bb_temp_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_health_invoke_extenders_for_test(root);

    bb_json_t temp = bb_json_obj_get_item(root, "temp");
    TEST_ASSERT_NOT_NULL(temp);

    double soc_c = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(temp, "soc_c", &soc_c));
    TEST_ASSERT_DOUBLE_WITHIN(0.05, 36.8, soc_c);

    bb_json_free(root);
}

void test_bb_temp_health_schema_fragment_present(void)
{
    bb_temp_register_info();
    const char *frag = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(frag);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(frag, "\"temp\""),
                                 "temp key not in health extender schema fragment");
    TEST_ASSERT_NOT_NULL(strstr(frag, "\"present\""));
    TEST_ASSERT_NOT_NULL(strstr(frag, "\"soc_c\""));
}
