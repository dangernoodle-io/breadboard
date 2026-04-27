#include "unity.h"
#include "bb_system.h"
#include <string.h>

void test_bb_system_get_version_returns_nonnull(void)
{
    const char *v = bb_system_get_version();
    TEST_ASSERT_NOT_NULL(v);
}

void test_bb_system_get_version_default_is_host_string(void)
{
    const char *v = bb_system_get_version();
    TEST_ASSERT_EQUAL_STRING("0.0.0-host", v);
}

void test_bb_system_get_project_name_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_project_name();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_system_get_build_date_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_build_date();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_system_get_build_time_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_build_time();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_system_get_idf_version_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_idf_version();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_error_check_happy_path(void)
{
    // BB_ERROR_CHECK with BB_OK must not abort
    BB_ERROR_CHECK(BB_OK);
    TEST_PASS();
}
