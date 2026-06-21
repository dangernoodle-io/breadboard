// Host tests for bb_vcore_wd pure vcore-collapse watchdog.
//
// Covers (100% branch coverage target):
//   - warmup suppression
//   - healthy path: no action, streak tracking, burst reset after healthy window
//   - marginal path (between COLLAPSE_MV and OK_MV): consec_low cleared, NONE
//   - collapse→RECOVER after BB_VCORE_WD_COLLAPSE_POLLS consecutive readings
//   - burst limit→BACKOFF after BB_VCORE_WD_BURST_MAX recoveries
//   - burst window expiry resets burst counter
//   - healthy window resets burst counter
//   - rail_disabled suppresses collapse action
//   - no trigger when vcore is healthy
//   - consec_low resets on healthy reading (after prior partial collapse count)
#include "unity.h"
#include "bb_power_health.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_vcore_wd_state_t s_st;
static bb_vcore_wd_input_t s_in;

static void reset(void)
{
    memset(&s_st, 0, sizeof(s_st));
    // Default: healthy rail, post-warmup.
    s_in.vcore_mv    = 1150;
    s_in.rail_enabled = true;
    s_in.uptime_ms   = BB_VCORE_WD_WARMUP_MS + 1000U;
}

static bb_vcore_wd_action_t eval(void)
{
    return bb_vcore_wd_eval(&s_st, &s_in);
}

// Bump uptime by delta_ms between calls.
static void advance_ms(uint64_t delta_ms)
{
    s_in.uptime_ms += delta_ms;
}

// Feed N collapse-level readings.
static bb_vcore_wd_action_t feed_collapse(int n)
{
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1; // clearly below threshold
    bb_vcore_wd_action_t last = BB_VCORE_WD_NONE;
    for (int i = 0; i < n; i++) {
        last = eval();
        advance_ms(1000);
    }
    return last;
}

// Feed N healthy readings.
static void feed_healthy(int n)
{
    s_in.vcore_mv = BB_VCORE_WD_OK_MV + 100;
    for (int i = 0; i < n; i++) {
        eval();
        advance_ms(1000);
    }
}

// ---------------------------------------------------------------------------
// Warmup suppression
// ---------------------------------------------------------------------------

void test_bb_vcore_wd_warmup_suppresses_all(void)
{
    reset();
    // Set uptime inside warmup window.
    s_in.uptime_ms = BB_VCORE_WD_WARMUP_MS / 2;
    s_in.vcore_mv  = 0; // collapsed
    s_in.rail_enabled = true;

    // Even feeding many collapsed readings inside warmup must return NONE.
    for (int i = 0; i < BB_VCORE_WD_COLLAPSE_POLLS + 1; i++) {
        bb_vcore_wd_action_t act = eval();
        TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, act);
        advance_ms(100);
    }
}

void test_bb_vcore_wd_warmup_boundary_exact_still_suppressed(void)
{
    reset();
    s_in.uptime_ms = BB_VCORE_WD_WARMUP_MS; // exactly at boundary — still in warmup
    s_in.vcore_mv  = 0;
    s_in.rail_enabled = true;
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, eval());
}

void test_bb_vcore_wd_warmup_one_ms_past_allows_detection(void)
{
    reset();
    // Post-warmup: a single healthy reading should return NONE (not a collapse).
    s_in.uptime_ms = BB_VCORE_WD_WARMUP_MS + 1U;
    s_in.vcore_mv  = 1150;
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, eval());
}

// ---------------------------------------------------------------------------
// Healthy path
// ---------------------------------------------------------------------------

void test_bb_vcore_wd_healthy_returns_none(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, eval());
}

void test_bb_vcore_wd_healthy_ok_mv_boundary(void)
{
    reset();
    s_in.vcore_mv = BB_VCORE_WD_OK_MV; // exactly at OK threshold
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, eval());
}

void test_bb_vcore_wd_healthy_clears_consec_low(void)
{
    reset();
    // Feed some (but fewer than COLLAPSE_POLLS) collapse readings to bump consec_low.
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1;
    for (int i = 0; i < BB_VCORE_WD_COLLAPSE_POLLS - 1; i++) {
        eval();
        advance_ms(1000);
    }
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_COLLAPSE_POLLS - 1, s_st.consec_low);

    // One healthy reading clears the counter.
    s_in.vcore_mv = BB_VCORE_WD_OK_MV + 50;
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, eval());
    TEST_ASSERT_EQUAL_INT(0, s_st.consec_low);
}

void test_bb_vcore_wd_healthy_window_resets_burst_counter(void)
{
    reset();
    // Trigger some recoveries to build up burst_count.
    for (int i = 0; i < BB_VCORE_WD_BURST_MAX; i++) {
        feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
        // Feed enough healthy readings to clear consec_low between bursts,
        // but NOT enough to reset the burst counter yet.
        feed_healthy(1);
    }
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_BURST_MAX, s_st.burst_count);

    // Now feed healthy readings for the full healthy-reset window.
    s_in.vcore_mv = BB_VCORE_WD_OK_MV + 100;
    // Advance past BB_VCORE_WD_HEALTHY_RESET_MS in one big step.
    advance_ms(BB_VCORE_WD_HEALTHY_RESET_MS + 1000U);
    bb_vcore_wd_action_t act = eval();

    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, act);
    TEST_ASSERT_EQUAL_INT(0, s_st.burst_count);
}

// ---------------------------------------------------------------------------
// Marginal path (COLLAPSE_MV <= vcore < OK_MV)
// ---------------------------------------------------------------------------

void test_bb_vcore_wd_marginal_returns_none_and_clears_consec_low(void)
{
    reset();
    // Bump consec_low with partial collapse.
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1;
    eval();
    advance_ms(1000);
    TEST_ASSERT_EQUAL_INT(1, s_st.consec_low);

    // Marginal reading: at exactly COLLAPSE_MV (not below).
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV;
    bb_vcore_wd_action_t act = eval();
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, act);
    // consec_low cleared by the marginal (not-below-collapse) branch.
    TEST_ASSERT_EQUAL_INT(0, s_st.consec_low);
}

// ---------------------------------------------------------------------------
// Collapse → RECOVER
// ---------------------------------------------------------------------------

void test_bb_vcore_wd_collapse_requires_n_polls(void)
{
    reset();
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1;

    // First COLLAPSE_POLLS-1 readings should return NONE.
    for (int i = 0; i < BB_VCORE_WD_COLLAPSE_POLLS - 1; i++) {
        bb_vcore_wd_action_t act = eval();
        TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, act);
        advance_ms(1000);
    }
    // The Nth reading triggers RECOVER.
    bb_vcore_wd_action_t act = eval();
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_RECOVER, act);
}

void test_bb_vcore_wd_collapse_recover_increments_burst(void)
{
    reset();
    feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
    TEST_ASSERT_EQUAL_INT(1, s_st.burst_count);
}

void test_bb_vcore_wd_collapse_recover_resets_consec_low(void)
{
    reset();
    feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
    TEST_ASSERT_EQUAL_INT(0, s_st.consec_low);
}

void test_bb_vcore_wd_collapse_rail_disabled_suppresses(void)
{
    reset();
    s_in.vcore_mv     = BB_VCORE_WD_COLLAPSE_MV - 1;
    s_in.rail_enabled = false;

    for (int i = 0; i < BB_VCORE_WD_COLLAPSE_POLLS + 1; i++) {
        bb_vcore_wd_action_t act = eval();
        TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, act);
        advance_ms(1000);
    }
}

// ---------------------------------------------------------------------------
// Burst limit → BACKOFF
// ---------------------------------------------------------------------------

void test_bb_vcore_wd_burst_max_triggers_backoff(void)
{
    reset();
    // Trigger BB_VCORE_WD_BURST_MAX recoveries.
    for (int i = 0; i < BB_VCORE_WD_BURST_MAX; i++) {
        bb_vcore_wd_action_t act = feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
        TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_RECOVER, act);
        // Clear consec_low with a brief healthy reading between bursts,
        // but stay within the burst window timeline.
        feed_healthy(1);
    }
    // Next collapse run should BACKOFF.
    bb_vcore_wd_action_t act = feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_BACKOFF, act);
}

void test_bb_vcore_wd_backoff_persists_without_healthy(void)
{
    reset();
    // Reach BACKOFF state.
    for (int i = 0; i < BB_VCORE_WD_BURST_MAX; i++) {
        feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
        feed_healthy(1);
    }
    feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_BURST_MAX, s_st.burst_count);

    // Additional collapsed readings inside the burst window keep returning BACKOFF.
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1;
    bb_vcore_wd_action_t act = eval();
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_BACKOFF, act);
}

void test_bb_vcore_wd_burst_window_expiry_resets_and_recovers(void)
{
    reset();
    // Reach BACKOFF state.
    for (int i = 0; i < BB_VCORE_WD_BURST_MAX; i++) {
        feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
        feed_healthy(1);
    }
    // Trigger BACKOFF: the final feed_collapse leaves consec_low at COLLAPSE_POLLS
    // (not reset in BACKOFF path).
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1;
    feed_collapse(BB_VCORE_WD_COLLAPSE_POLLS);
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_BURST_MAX, s_st.burst_count);
    // consec_low is still >= COLLAPSE_POLLS here (not reset on BACKOFF).

    // Advance past the burst window so it expires on the next entry.
    advance_ms(BB_VCORE_WD_WINDOW_MS + 1000U);

    // A single collapse eval now triggers the window-expiry reset path and
    // immediately returns RECOVER (consec_low is already >= COLLAPSE_POLLS).
    s_in.vcore_mv = BB_VCORE_WD_COLLAPSE_MV - 1;
    bb_vcore_wd_action_t act = eval();
    TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_RECOVER, act);
    TEST_ASSERT_EQUAL_INT(1, s_st.burst_count);
}

// ---------------------------------------------------------------------------
// No trigger when healthy — make sure repeated healthy evals never fire
// ---------------------------------------------------------------------------

void test_bb_vcore_wd_no_action_when_always_healthy(void)
{
    reset();
    for (int i = 0; i < 100; i++) {
        bb_vcore_wd_action_t act = eval();
        TEST_ASSERT_EQUAL_INT(BB_VCORE_WD_NONE, act);
        advance_ms(1000);
    }
    TEST_ASSERT_EQUAL_INT(0, s_st.burst_count);
}
