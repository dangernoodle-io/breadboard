#include "unity.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"

static int s_extender_call_count = 0;

static void test_extender_fn(bb_json_t root)
{
    (void)root;
    s_extender_call_count++;
}

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
