#include "unity.h"
#include "bb_system.h"
#include "bb_system_test.h"
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

void test_bb_system_read_temp_default_unsupported(void)
{
    // host default: no sensor, must return BB_ERR_UNSUPPORTED
    bb_system_set_temp_for_test(0.0f, BB_ERR_UNSUPPORTED);
    float f = -999.0f;
    bb_err_t rc = bb_system_read_temp_celsius(&f);
    TEST_ASSERT_EQUAL_INT(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_FLOAT(-999.0f, f);  // *out untouched on error
}

void test_bb_system_read_temp_null_out(void)
{
    bb_err_t rc = bb_system_read_temp_celsius(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_system_read_temp_injected_ok(void)
{
    bb_system_set_temp_for_test(42.5f, BB_OK);
    float f = 0.0f;
    bb_err_t rc = bb_system_read_temp_celsius(&f);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_FLOAT(42.5f, f);
}

void test_bb_system_read_temp_injected_error(void)
{
    bb_system_set_temp_for_test(0.0f, BB_ERR_UNSUPPORTED);
    float f = -1.0f;
    bb_err_t rc = bb_system_read_temp_celsius(&f);
    TEST_ASSERT_EQUAL_INT(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, f);  // *out untouched on error
}

// Byte-identical value guard (B1-463): bb_system_reset_reason_str is driven by
// the shared BB_RESET_REASON_LIST X-macro across espidf/host/arduino. These
// assertions pin the exact wire strings so the X-macro can never silently drift.
void test_bb_system_reset_reason_str_poweron(void)
{
    TEST_ASSERT_EQUAL_STRING("power-on", bb_system_reset_reason_str(BB_RESET_REASON_POWERON));
}

void test_bb_system_reset_reason_str_ext(void)
{
    TEST_ASSERT_EQUAL_STRING("ext", bb_system_reset_reason_str(BB_RESET_REASON_EXT));
}

void test_bb_system_reset_reason_str_sw(void)
{
    TEST_ASSERT_EQUAL_STRING("software", bb_system_reset_reason_str(BB_RESET_REASON_SW));
}

void test_bb_system_reset_reason_str_panic(void)
{
    TEST_ASSERT_EQUAL_STRING("panic", bb_system_reset_reason_str(BB_RESET_REASON_PANIC));
}

void test_bb_system_reset_reason_str_int_wdt(void)
{
    TEST_ASSERT_EQUAL_STRING("int_wdt", bb_system_reset_reason_str(BB_RESET_REASON_INT_WDT));
}

void test_bb_system_reset_reason_str_task_wdt(void)
{
    TEST_ASSERT_EQUAL_STRING("task_wdt", bb_system_reset_reason_str(BB_RESET_REASON_TASK_WDT));
}

void test_bb_system_reset_reason_str_wdt(void)
{
    TEST_ASSERT_EQUAL_STRING("wdt", bb_system_reset_reason_str(BB_RESET_REASON_WDT));
}

void test_bb_system_reset_reason_str_deepsleep(void)
{
    TEST_ASSERT_EQUAL_STRING("deep_sleep", bb_system_reset_reason_str(BB_RESET_REASON_DEEPSLEEP));
}

void test_bb_system_reset_reason_str_brownout(void)
{
    TEST_ASSERT_EQUAL_STRING("brownout", bb_system_reset_reason_str(BB_RESET_REASON_BROWNOUT));
}

void test_bb_system_reset_reason_str_sdio(void)
{
    TEST_ASSERT_EQUAL_STRING("sdio", bb_system_reset_reason_str(BB_RESET_REASON_SDIO));
}

void test_bb_system_reset_reason_str_unknown(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_system_reset_reason_str(BB_RESET_REASON_UNKNOWN));
}

void test_bb_system_reset_reason_str_out_of_range(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_system_reset_reason_str((bb_reset_reason_t)999));
}
