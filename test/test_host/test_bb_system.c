#include "unity.h"
#include "bb_system.h"

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
