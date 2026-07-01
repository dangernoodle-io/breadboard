#include "unity.h"
#include "bb_ota_hooks.h"

// ---------------------------------------------------------------------------
// Helpers shared across groups
// ---------------------------------------------------------------------------

static void reset_hooks(void)
{
    bb_ota_hooks_test_reset();
}

// ---------------------------------------------------------------------------
// bb_ota_progress_json tests
// ---------------------------------------------------------------------------

void test_ota_hooks_json_push_start(void)
{
    char buf[128];
    int n = bb_ota_progress_json(buf, sizeof(buf), "push", 0, 0);
    TEST_ASSERT_EQUAL_STRING("{\"via\":\"push\",\"state\":\"start\",\"pct\":0}", buf);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_ota_hooks_json_pull_progress_42(void)
{
    char buf[128];
    int n = bb_ota_progress_json(buf, sizeof(buf), "pull", 1, 42);
    TEST_ASSERT_EQUAL_STRING("{\"via\":\"pull\",\"state\":\"progress\",\"pct\":42}", buf);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_ota_hooks_json_success(void)
{
    char buf[128];
    int n = bb_ota_progress_json(buf, sizeof(buf), "boot", 2, 100);
    TEST_ASSERT_EQUAL_STRING("{\"via\":\"boot\",\"state\":\"success\",\"pct\":100}", buf);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_ota_hooks_json_fail(void)
{
    char buf[128];
    int n = bb_ota_progress_json(buf, sizeof(buf), "push", 3, 0);
    TEST_ASSERT_EQUAL_STRING("{\"via\":\"push\",\"state\":\"fail\",\"pct\":0}", buf);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_ota_hooks_json_out_of_range_state_unknown(void)
{
    char buf[128];
    int n = bb_ota_progress_json(buf, sizeof(buf), "push", 99, 0);
    TEST_ASSERT_EQUAL_STRING("{\"via\":\"push\",\"state\":\"unknown\",\"pct\":0}", buf);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_ota_hooks_json_tiny_buf_returns_zero(void)
{
    char buf[4];
    int n = bb_ota_progress_json(buf, sizeof(buf), "push", 0, 0);
    TEST_ASSERT_EQUAL_INT(0, n);
}

// ---------------------------------------------------------------------------
// Progress callback + bb_ota_emit_progress + bb_ota_last_progress
// ---------------------------------------------------------------------------

static bb_ota_phase_t s_cb_phase;
static int            s_cb_pct;
static int            s_cb_calls;

static void test_progress_cb(bb_ota_phase_t phase, int pct)
{
    s_cb_phase = phase;
    s_cb_pct   = pct;
    s_cb_calls++;
}

void test_ota_hooks_emit_fires_cb(void)
{
    reset_hooks();
    s_cb_calls = 0;
    bb_ota_set_progress_cb(test_progress_cb);
    bb_ota_emit_progress("push", BB_OTA_PHASE_PROGRESS, 50);
    TEST_ASSERT_EQUAL_INT(1, s_cb_calls);
    TEST_ASSERT_EQUAL_INT(BB_OTA_PHASE_PROGRESS, (int)s_cb_phase);
    TEST_ASSERT_EQUAL_INT(50, s_cb_pct);
    reset_hooks();
}

void test_ota_hooks_last_progress_returns_last_emit(void)
{
    reset_hooks();
    bb_ota_set_progress_cb(test_progress_cb);
    bb_ota_emit_progress("pull", BB_OTA_PHASE_START, 0);
    bb_ota_emit_progress("pull", BB_OTA_PHASE_PROGRESS, 30);
    bb_ota_emit_progress("pull", BB_OTA_PHASE_SUCCESS, 100);

    bb_ota_phase_t phase;
    int pct;
    bb_ota_last_progress(&phase, &pct);
    TEST_ASSERT_EQUAL_INT(BB_OTA_PHASE_SUCCESS, (int)phase);
    TEST_ASSERT_EQUAL_INT(100, pct);
    reset_hooks();
}

void test_ota_hooks_multiple_emits_update_last(void)
{
    reset_hooks();
    bb_ota_emit_progress("boot", BB_OTA_PHASE_PROGRESS, 10);
    bb_ota_emit_progress("boot", BB_OTA_PHASE_PROGRESS, 70);

    bb_ota_phase_t phase;
    int pct;
    bb_ota_last_progress(&phase, &pct);
    TEST_ASSERT_EQUAL_INT(BB_OTA_PHASE_PROGRESS, (int)phase);
    TEST_ASSERT_EQUAL_INT(70, pct);
}

// ---------------------------------------------------------------------------
// Pause / resume
// ---------------------------------------------------------------------------

static bool s_pause_called;
static bool s_pause_result;
static bool s_resume_called;

static bool test_pause_cb(void)
{
    s_pause_called = true;
    return s_pause_result;
}

static void test_resume_cb(void)
{
    s_resume_called = true;
}

void test_ota_hooks_pause_returns_true_when_cb_returns_true(void)
{
    reset_hooks();
    s_pause_called = false;
    s_pause_result = true;
    bb_ota_set_hooks(test_pause_cb, NULL);
    TEST_ASSERT_TRUE(bb_ota_pause());
    TEST_ASSERT_TRUE(s_pause_called);
    reset_hooks();
}

void test_ota_hooks_pause_returns_false_when_no_cb(void)
{
    reset_hooks();
    TEST_ASSERT_FALSE(bb_ota_pause());
}

void test_ota_hooks_resume_calls_cb(void)
{
    reset_hooks();
    s_resume_called = false;
    bb_ota_set_hooks(NULL, test_resume_cb);
    bb_ota_resume();
    TEST_ASSERT_TRUE(s_resume_called);
    reset_hooks();
}

void test_ota_hooks_resume_no_crash_when_null(void)
{
    reset_hooks();
    bb_ota_resume();  // must not crash
    TEST_ASSERT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Skip-check callback
// ---------------------------------------------------------------------------

static bool s_skip_result;

static bool test_skip_cb(void)
{
    return s_skip_result;
}

void test_ota_hooks_skip_check_returns_true(void)
{
    reset_hooks();
    s_skip_result = true;
    bb_ota_set_skip_check_cb(test_skip_cb);
    TEST_ASSERT_TRUE(bb_ota_skip_check());
    reset_hooks();
}

void test_ota_hooks_skip_check_returns_false(void)
{
    reset_hooks();
    s_skip_result = false;
    bb_ota_set_skip_check_cb(test_skip_cb);
    TEST_ASSERT_FALSE(bb_ota_skip_check());
    reset_hooks();
}

void test_ota_hooks_skip_check_null_returns_false(void)
{
    reset_hooks();
    TEST_ASSERT_FALSE(bb_ota_skip_check());
}

// ---------------------------------------------------------------------------
// bb_ota_has_pause_hook
// ---------------------------------------------------------------------------

void test_ota_hooks_has_pause_hook_reflects_set(void)
{
    reset_hooks();
    TEST_ASSERT_FALSE(bb_ota_has_pause_hook());

    bb_ota_set_hooks(test_pause_cb, NULL);
    TEST_ASSERT_TRUE(bb_ota_has_pause_hook());

    reset_hooks();
    TEST_ASSERT_FALSE(bb_ota_has_pause_hook());
}

// ---------------------------------------------------------------------------
// Multi-consumer registration (B1-446) — append, not overwrite
// ---------------------------------------------------------------------------

static bool s_pause_result_2;
static int  s_pause_order[2];
static int  s_pause_order_n;
static int  s_resume_order[2];
static int  s_resume_order_n;

static bool test_pause_cb_ordered_1(void)
{
    s_pause_order[s_pause_order_n++] = 1;
    return s_pause_result;
}

static bool test_pause_cb_ordered_2(void)
{
    s_pause_order[s_pause_order_n++] = 2;
    return s_pause_result_2;
}

static void test_resume_cb_ordered_1(void)
{
    s_resume_order[s_resume_order_n++] = 1;
}

static void test_resume_cb_ordered_2(void)
{
    s_resume_order[s_resume_order_n++] = 2;
}

void test_ota_hooks_dual_registration_both_pause_and_resume_fire(void)
{
    reset_hooks();
    s_pause_called    = false;
    s_pause_order_n   = 0;
    s_resume_order_n  = 0;
    s_pause_result    = false;
    s_pause_result_2  = false;

    bb_ota_set_hooks(test_pause_cb, test_resume_cb_ordered_1);
    bb_ota_set_hooks(test_pause_cb_ordered_2, test_resume_cb_ordered_2);
    TEST_ASSERT_EQUAL_size_t(2, bb_ota_hooks_pause_count());

    bb_ota_pause();
    TEST_ASSERT_TRUE(s_pause_called);       // 1st registrant fired
    TEST_ASSERT_EQUAL_INT(1, s_pause_order_n); // 2nd registrant fired too

    bb_ota_resume();
    TEST_ASSERT_EQUAL_INT(2, s_resume_order_n); // both resumes fired
    reset_hooks();
}

void test_ota_hooks_pause_registration_order_preserved(void)
{
    reset_hooks();
    s_pause_order_n = 0;
    s_pause_result  = false;
    s_pause_result_2 = false;

    bb_ota_set_hooks(test_pause_cb_ordered_1, test_resume_cb_ordered_1);
    bb_ota_set_hooks(test_pause_cb_ordered_2, test_resume_cb_ordered_2);
    bb_ota_pause();
    TEST_ASSERT_EQUAL_INT(2, s_pause_order_n);
    TEST_ASSERT_EQUAL_INT(1, s_pause_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_pause_order[1]);

    s_resume_order_n = 0;
    bb_ota_resume();
    TEST_ASSERT_EQUAL_INT(2, s_resume_order_n);
    TEST_ASSERT_EQUAL_INT(1, s_resume_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_resume_order[1]);
    reset_hooks();
}

void test_ota_hooks_pause_combine_true_if_any_true(void)
{
    reset_hooks();
    s_pause_result   = false;
    s_pause_result_2 = true;
    bb_ota_set_hooks(test_pause_cb, NULL);
    bb_ota_set_hooks(test_pause_cb_ordered_2, NULL);
    TEST_ASSERT_TRUE(bb_ota_pause());
    reset_hooks();
}

void test_ota_hooks_pause_registration_overflow_dropped(void)
{
    reset_hooks();
    for (int i = 0; i < BB_OTA_HOOKS_MAX; i++) {
        bb_ota_set_hooks(test_pause_cb, test_resume_cb);
    }
    TEST_ASSERT_EQUAL_size_t(BB_OTA_HOOKS_MAX, bb_ota_hooks_pause_count());

    // one more beyond cap — dropped, count stays at cap.
    bb_ota_set_hooks(test_pause_cb, test_resume_cb);
    TEST_ASSERT_EQUAL_size_t(BB_OTA_HOOKS_MAX, bb_ota_hooks_pause_count());
    reset_hooks();
}

void test_ota_hooks_progress_registration_overflow_dropped(void)
{
    reset_hooks();
    for (int i = 0; i < BB_OTA_HOOKS_MAX; i++) {
        bb_ota_set_progress_cb(test_progress_cb);
    }
    TEST_ASSERT_EQUAL_size_t(BB_OTA_HOOKS_MAX, bb_ota_hooks_progress_count());

    bb_ota_set_progress_cb(test_progress_cb);
    TEST_ASSERT_EQUAL_size_t(BB_OTA_HOOKS_MAX, bb_ota_hooks_progress_count());
    reset_hooks();
}

void test_ota_hooks_dual_progress_both_fire(void)
{
    reset_hooks();
    s_cb_calls = 0;
    bb_ota_set_progress_cb(test_progress_cb);
    bb_ota_set_progress_cb(test_progress_cb);
    bb_ota_emit_progress("push", BB_OTA_PHASE_PROGRESS, 25);
    TEST_ASSERT_EQUAL_INT(2, s_cb_calls);
    reset_hooks();
}

void test_ota_hooks_skip_check_registration_overflow_dropped(void)
{
    reset_hooks();
    for (int i = 0; i < BB_OTA_HOOKS_MAX; i++) {
        bb_ota_set_skip_check_cb(test_skip_cb);
    }
    TEST_ASSERT_EQUAL_size_t(BB_OTA_HOOKS_MAX, bb_ota_hooks_skip_check_count());

    bb_ota_set_skip_check_cb(test_skip_cb);
    TEST_ASSERT_EQUAL_size_t(BB_OTA_HOOKS_MAX, bb_ota_hooks_skip_check_count());
    reset_hooks();
}

static bool s_skip_result_2;

static bool test_skip_cb_2(void)
{
    return s_skip_result_2;
}

void test_ota_hooks_skip_check_combine_true_if_any_true(void)
{
    reset_hooks();
    s_skip_result   = false;
    s_skip_result_2 = true;
    bb_ota_set_skip_check_cb(test_skip_cb);
    bb_ota_set_skip_check_cb(test_skip_cb_2);
    TEST_ASSERT_TRUE(bb_ota_skip_check());
    reset_hooks();
}

void test_ota_hooks_skip_check_combine_false_if_all_false(void)
{
    reset_hooks();
    s_skip_result   = false;
    s_skip_result_2 = false;
    bb_ota_set_skip_check_cb(test_skip_cb);
    bb_ota_set_skip_check_cb(test_skip_cb_2);
    TEST_ASSERT_FALSE(bb_ota_skip_check());
    reset_hooks();
}

static int s_skip_side_effect_calls;

static bool test_skip_cb_side_effect(void)
{
    s_skip_side_effect_calls++;
    return false;
}

// A true-returning hook registered FIRST must not short-circuit later
// registrants — every hook fires exactly once (mirrors bb_ota_pause).
void test_ota_hooks_skip_check_all_hooks_fire_even_after_true(void)
{
    reset_hooks();
    s_skip_side_effect_calls = 0;
    s_skip_result            = true;
    bb_ota_set_skip_check_cb(test_skip_cb);           // true, registered first
    bb_ota_set_skip_check_cb(test_skip_cb_side_effect); // registered second
    TEST_ASSERT_TRUE(bb_ota_skip_check());
    TEST_ASSERT_EQUAL_INT(1, s_skip_side_effect_calls);
    reset_hooks();
}
