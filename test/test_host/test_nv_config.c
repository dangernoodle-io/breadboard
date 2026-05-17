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

void test_nv_config_mdns_enabled_default_true(void)
{
    bb_nv_config_init();
    bool enabled = bb_nv_config_mdns_enabled();
    TEST_ASSERT_TRUE(enabled);
}

void test_nv_config_is_provisioned_stub_returns_false(void)
{
    bb_nv_config_init();
    bool provisioned = bb_nv_config_is_provisioned();
    TEST_ASSERT_FALSE(provisioned);
}

void test_hostname_default_empty(void)
{
    bb_nv_config_init();
    const char *hostname = bb_nv_config_hostname();
    TEST_ASSERT_NOT_NULL(hostname);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_hostname_set_get_roundtrip(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("tdongle-s3-1");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    const char *hostname = bb_nv_config_hostname();
    TEST_ASSERT_EQUAL_STRING("tdongle-s3-1", hostname);
}

void test_hostname_validates_charset(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("bad host!");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_validates_length(void)
{
    bb_nv_config_init();
    // 33 character string exceeds max of 32
    bb_err_t err = bb_nv_config_set_hostname("012345678901234567890123456789012");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_rejects_leading_hyphen(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("-foo");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_rejects_trailing_hyphen(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("foo-");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_rejects_null(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}
