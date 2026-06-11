// Host unit tests for bb_diag diag.boot event topic:
// - bb_diag_boot_build_json (pure JSON builder)
#include "unity.h"
#include "bb_diag_event_priv.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// bb_diag_boot_build_json — normal cases
// ---------------------------------------------------------------------------

void test_bb_diag_boot_build_json_poweron_clean(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "poweron", 0, false, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"poweron\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"abnormal_reset_count\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panic_available\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rolled_back\":false"));
}

void test_bb_diag_boot_build_json_panic_with_count(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "panic", 3, true, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"panic\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"abnormal_reset_count\":3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panic_available\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rolled_back\":false"));
}

void test_bb_diag_boot_build_json_task_wdt(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "task_wdt", 7, true, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"task_wdt\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"abnormal_reset_count\":7"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panic_available\":true"));
}

void test_bb_diag_boot_build_json_rolled_back_true(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "software", 0, false, true);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rolled_back\":true"));
}

void test_bb_diag_boot_build_json_all_flags_true(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "brownout", 42, true, true);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"brownout\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"abnormal_reset_count\":42"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panic_available\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rolled_back\":true"));
}

void test_bb_diag_boot_build_json_zero_count(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "poweron", 0, false, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"abnormal_reset_count\":0"));
}

void test_bb_diag_boot_build_json_large_count(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "int_wdt", 4294967295U, false, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"int_wdt\""));
    // UINT32_MAX should be encoded correctly
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"abnormal_reset_count\":4294967295"));
}

// ---------------------------------------------------------------------------
// Defensive: bad args
// ---------------------------------------------------------------------------

void test_bb_diag_boot_build_json_null_buf_returns_neg1(void)
{
    int n = bb_diag_boot_build_json(NULL, 256, "poweron", 0, false, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_diag_boot_build_json_zero_buf_sz_returns_neg1(void)
{
    char buf[4];
    int n = bb_diag_boot_build_json(buf, 0, "poweron", 0, false, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_diag_boot_build_json_null_reset_reason_returns_neg1(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), NULL, 0, false, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

// ---------------------------------------------------------------------------
// Output is valid JSON structure (spot-check nesting)
// ---------------------------------------------------------------------------

void test_bb_diag_boot_build_json_starts_with_brace(void)
{
    char buf[256];
    bb_diag_boot_build_json(buf, sizeof(buf), "poweron", 0, false, false);
    TEST_ASSERT_EQUAL_CHAR('{', buf[0]);
}

void test_bb_diag_boot_build_json_ends_with_brace(void)
{
    char buf[256];
    int n = bb_diag_boot_build_json(buf, sizeof(buf), "poweron", 0, false, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    // n <= sizeof(buf)-1 since snprintf wrote into buf
    int len = (int)strlen(buf);
    TEST_ASSERT_EQUAL_CHAR('}', buf[len - 1]);
}
