#include "unity.h"
#include "bb_nv_wifi_pending.h"
#include <string.h>

/* --- bb_wifi_pending_decide --- */

void test_wifi_pending_decide_try0_with_ssid_returns_none(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PENDING_NONE,
                      bb_wifi_pending_decide(0, "mynet"));
}

void test_wifi_pending_decide_try1_empty_ssid_returns_none(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PENDING_NONE,
                      bb_wifi_pending_decide(1, ""));
}

void test_wifi_pending_decide_try1_null_ssid_returns_none(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PENDING_NONE,
                      bb_wifi_pending_decide(1, NULL));
}

void test_wifi_pending_decide_try1_valid_ssid_returns_try(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PENDING_TRY,
                      bb_wifi_pending_decide(1, "net"));
}

/* --- bb_wifi_pending_validate --- */

void test_wifi_pending_validate_null_ssid_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_wifi_pending_validate(NULL, "pass"));
}

void test_wifi_pending_validate_empty_ssid_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_wifi_pending_validate("", "pass"));
}

void test_wifi_pending_validate_ssid_at_max_returns_ok(void)
{
    /* BB_WIFI_PENDING_SSID_MAX == 31; exactly 31 chars must be OK */
    char ssid[BB_WIFI_PENDING_SSID_MAX + 1];
    memset(ssid, 'A', BB_WIFI_PENDING_SSID_MAX);
    ssid[BB_WIFI_PENDING_SSID_MAX] = '\0';

    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_pending_validate(ssid, "pass"));
}

void test_wifi_pending_validate_ssid_over_max_returns_invalid_arg(void)
{
    /* 32 chars — one over the limit */
    char ssid[BB_WIFI_PENDING_SSID_MAX + 2];
    memset(ssid, 'A', BB_WIFI_PENDING_SSID_MAX + 1);
    ssid[BB_WIFI_PENDING_SSID_MAX + 1] = '\0';

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_wifi_pending_validate(ssid, "pass"));
}

void test_wifi_pending_validate_pass_at_max_returns_ok(void)
{
    /* BB_WIFI_PENDING_PASS_MAX == 63; exactly 63 chars must be OK */
    char pass[BB_WIFI_PENDING_PASS_MAX + 1];
    memset(pass, 'B', BB_WIFI_PENDING_PASS_MAX);
    pass[BB_WIFI_PENDING_PASS_MAX] = '\0';

    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_pending_validate("net", pass));
}

void test_wifi_pending_validate_pass_over_max_returns_invalid_arg(void)
{
    /* 64 chars — one over the limit */
    char pass[BB_WIFI_PENDING_PASS_MAX + 2];
    memset(pass, 'B', BB_WIFI_PENDING_PASS_MAX + 1);
    pass[BB_WIFI_PENDING_PASS_MAX + 1] = '\0';

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_wifi_pending_validate("net", pass));
}

void test_wifi_pending_validate_null_pass_returns_ok(void)
{
    /* NULL pass = open network; must be accepted */
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_pending_validate("net", NULL));
}

void test_wifi_pending_validate_empty_pass_returns_ok(void)
{
    /* Empty pass = open network; must be accepted */
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_pending_validate("net", ""));
}
