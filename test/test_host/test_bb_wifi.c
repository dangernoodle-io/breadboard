#include "unity.h"
#include "bb_wifi.h"

#ifdef BB_WIFI_TESTING
#include "bb_wifi_test.h"
#endif

void test_bb_wifi_set_hostname_null(void)
{
    bb_err_t err = bb_wifi_set_hostname(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_set_hostname_empty(void)
{
    bb_err_t err = bb_wifi_set_hostname("");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_set_hostname_valid(void)
{
    bb_err_t err = bb_wifi_set_hostname("valid-host");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// Test: no-ip no-op
void test_bb_wifi_request_recovery_no_ip_noop(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(false);
    bb_err_t err = bb_wifi_request_recovery("test");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_wifi_test_get_recovery_count()); // no-op
#endif
}

// Test: has-ip triggers recovery
void test_bb_wifi_request_recovery_triggers_restart(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(true);
    bb_err_t err = bb_wifi_request_recovery("stratum_dead");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_wifi_test_get_recovery_count());
    TEST_ASSERT_NOT_NULL(bb_wifi_test_get_last_recovery_reason());
    TEST_ASSERT_EQUAL_STRING("stratum_dead", bb_wifi_test_get_last_recovery_reason());
#endif
}

// Test: null reason is safe
void test_bb_wifi_request_recovery_null_reason(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(true);
    bb_err_t err = bb_wifi_request_recovery(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_wifi_test_get_recovery_count());
#endif
}
