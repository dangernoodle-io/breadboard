#include "unity.h"
#include "nv_config.h"
#include <string.h>

void test_nv_config_init_success(void)
{
    bsp_err_t err = bsp_nv_config_init();
    TEST_ASSERT_EQUAL_INT(0, err);
}

void test_nv_config_wifi_ssid_empty_after_init(void)
{
    bsp_nv_config_init();
    const char *ssid = bsp_nv_config_wifi_ssid();
    TEST_ASSERT_NOT_NULL(ssid);
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_nv_config_wifi_pass_empty_after_init(void)
{
    bsp_nv_config_init();
    const char *pass = bsp_nv_config_wifi_pass();
    TEST_ASSERT_NOT_NULL(pass);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

void test_nv_config_display_enabled_default_true(void)
{
    bsp_nv_config_init();
    bool enabled = bsp_nv_config_display_enabled();
    TEST_ASSERT_TRUE(enabled);
}

void test_nv_config_is_provisioned_stub_returns_false(void)
{
    bsp_nv_config_init();
    bool provisioned = bsp_nv_config_is_provisioned();
    TEST_ASSERT_FALSE(provisioned);
}
