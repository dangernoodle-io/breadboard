#include "unity.h"
#include "bb_ntp_info.h"
#include "bb_ntp.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "bb_json.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Tests: bb_ntp_info schema fragment
 * --------------------------------------------------------------------------- */

void test_bb_ntp_info_schema_in_assembled_schema(void)
{
    bb_ntp_register_info();
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"ntp\""),
                                 "ntp key not in assembled schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"synced\""),
                                 "synced key not in assembled schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"last_sync_unix\""),
                                 "last_sync_unix key not in assembled schema");
}

/* ---------------------------------------------------------------------------
 * Tests: bb_ntp_info extender output
 * --------------------------------------------------------------------------- */

void test_bb_ntp_info_extender_synced_false_last_sync_zero(void)
{
    /* Host stub: bb_ntp_is_synced() returns false, bb_ntp_last_sync_unix() = 0 */
    bb_ntp_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t ntp = bb_json_obj_get_item(root, "ntp");
    TEST_ASSERT_NOT_NULL_MESSAGE(ntp, "ntp key missing from extender output");

    bool synced = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(ntp, "synced", &synced));
    TEST_ASSERT_FALSE(synced);

    double last_sync = -1.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(ntp, "last_sync_unix", &last_sync));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, last_sync);

    bb_json_free(root);
}

void test_bb_ntp_info_extender_ntp_key_always_present(void)
{
    /* Verify "ntp" key is always emitted regardless of sync state */
    bb_ntp_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t ntp = bb_json_obj_get_item(root, "ntp");
    TEST_ASSERT_NOT_NULL_MESSAGE(ntp, "ntp object must always be present");

    bb_json_free(root);
}
