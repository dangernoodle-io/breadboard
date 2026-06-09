#include "unity.h"
#include "bb_ota_hooks.h"

// ---------------------------------------------------------------------------
// Helpers shared across groups
// ---------------------------------------------------------------------------

static void reset_hooks(void)
{
    bb_ota_set_hooks(NULL, NULL);
    bb_ota_set_progress_cb(NULL);
    bb_ota_set_skip_check_cb(NULL);
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
    bb_ota_set_hooks(NULL, NULL);
    TEST_ASSERT_FALSE(bb_ota_has_pause_hook());

    bb_ota_set_hooks(test_pause_cb, NULL);
    TEST_ASSERT_TRUE(bb_ota_has_pause_hook());

    bb_ota_set_hooks(NULL, NULL);
    TEST_ASSERT_FALSE(bb_ota_has_pause_hook());
}
