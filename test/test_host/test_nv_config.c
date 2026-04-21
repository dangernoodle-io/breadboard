#include "unity.h"
#include "bb_nv.h"
#include <string.h>

void test_nv_config_init_success(void)
{
    bb_err_t err = bb_nv_config_init();
    TEST_ASSERT_EQUAL_INT(0, err);
}

void test_nv_config_wifi_ssid_empty_after_init(void)
{
    bb_nv_config_init();
    const char *ssid = bb_nv_config_wifi_ssid();
    TEST_ASSERT_NOT_NULL(ssid);
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_nv_config_wifi_pass_empty_after_init(void)
{
    bb_nv_config_init();
    const char *pass = bb_nv_config_wifi_pass();
    TEST_ASSERT_NOT_NULL(pass);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

void test_nv_config_display_enabled_default_true(void)
{
    bb_nv_config_init();
    bool enabled = bb_nv_config_display_enabled();
    TEST_ASSERT_TRUE(enabled);
}

void test_nv_config_is_provisioned_stub_returns_false(void)
{
    bb_nv_config_init();
    bool provisioned = bb_nv_config_is_provisioned();
    TEST_ASSERT_FALSE(provisioned);
}
