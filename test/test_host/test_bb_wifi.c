#include "unity.h"
#include "bb_wifi.h"

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
