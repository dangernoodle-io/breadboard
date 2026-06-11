// Host unit tests for bb_display_info health.display event topic:
// - bb_display_info_event_build_json (pure JSON builder)
#include "unity.h"
#include "bb_display_info_event_priv.h"

#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// bb_display_info_event_build_json — present=true with panel
// ---------------------------------------------------------------------------

void test_bb_display_info_event_build_json_present_true(void)
{
    char buf[256];
    int n = bb_display_info_event_build_json(buf, sizeof(buf), true, "ek79007", NULL);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panel\":\"ek79007\""));
    // reason must not appear when present=true
    TEST_ASSERT_NULL(strstr(buf, "\"reason\""));
}

void test_bb_display_info_event_build_json_present_true_other_panel(void)
{
    char buf[256];
    int n = bb_display_info_event_build_json(buf, sizeof(buf), true, "ssd1306", NULL);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panel\":\"ssd1306\""));
}

// ---------------------------------------------------------------------------
// bb_display_info_event_build_json — present=false with reason
// ---------------------------------------------------------------------------

void test_bb_display_info_event_build_json_present_false_with_reason(void)
{
    char buf[256];
    int n = bb_display_info_event_build_json(buf, sizeof(buf), false, NULL, "no backend");
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"no backend\""));
    // panel must not appear when present=false
    TEST_ASSERT_NULL(strstr(buf, "\"panel\""));
}

void test_bb_display_info_event_build_json_present_false_init_failed(void)
{
    char buf[256];
    int n = bb_display_info_event_build_json(buf, sizeof(buf), false, NULL, "init failed");
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"init failed\""));
}

void test_bb_display_info_event_build_json_present_false_no_reason(void)
{
    // reason=NULL is allowed: emits {"present":false} without reason key
    char buf[256];
    int n = bb_display_info_event_build_json(buf, sizeof(buf), false, NULL, NULL);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":false"));
    TEST_ASSERT_NULL(strstr(buf, "\"reason\""));
}

// ---------------------------------------------------------------------------
// Defensive: bad args
// ---------------------------------------------------------------------------

void test_bb_display_info_event_build_json_null_buf_returns_neg1(void)
{
    int n = bb_display_info_event_build_json(NULL, 256, true, "panel", NULL);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_display_info_event_build_json_zero_buf_sz_returns_neg1(void)
{
    char buf[4];
    int n = bb_display_info_event_build_json(buf, 0, true, "panel", NULL);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_display_info_event_build_json_present_true_null_panel_returns_neg1(void)
{
    // present=true but panel=NULL is an error
    char buf[256];
    int n = bb_display_info_event_build_json(buf, sizeof(buf), true, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, n);
}
