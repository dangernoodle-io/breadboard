#include "unity.h"
#include "bb_log_internal.h"

// ---------------------------------------------------------------------------
// bb_log_flush_seq_reached — wraparound-safe "has this marker completed" check
// ---------------------------------------------------------------------------

void test_bb_log_flush_seq_reached_exact_match(void) {
    TEST_ASSERT_TRUE(bb_log_flush_seq_reached(5, 5));
}

void test_bb_log_flush_seq_reached_completed_ahead(void) {
    // completed_seq > target_seq: still "reached" (>= semantics).
    TEST_ASSERT_TRUE(bb_log_flush_seq_reached(7, 5));
}

void test_bb_log_flush_seq_reached_completed_behind(void) {
    // The stale-give case: an earlier/abandoned marker was drained, not ours.
    TEST_ASSERT_FALSE(bb_log_flush_seq_reached(3, 5));
}

void test_bb_log_flush_seq_reached_zero_not_reached_by_default(void) {
    TEST_ASSERT_FALSE(bb_log_flush_seq_reached(0, 1));
}

void test_bb_log_flush_seq_reached_wraparound_just_past(void) {
    // target_seq is the last value before uint32_t wraps; completed_seq has
    // wrapped to 0 -- that's one past target_seq in modular arithmetic, so
    // it must read as reached.
    TEST_ASSERT_TRUE(bb_log_flush_seq_reached(0, UINT32_MAX));
}

void test_bb_log_flush_seq_reached_wraparound_not_yet(void) {
    // completed_seq is still one behind target_seq, both near the wrap
    // boundary -- must NOT read as reached.
    TEST_ASSERT_FALSE(bb_log_flush_seq_reached(UINT32_MAX - 1, UINT32_MAX));
}

// ---------------------------------------------------------------------------
// bb_log_flush_wait_decide — one wait-loop iteration's decision
// ---------------------------------------------------------------------------

void test_bb_log_flush_wait_decide_take_failed_is_timeout(void) {
    // A failed take is always a timeout regardless of the seq values.
    TEST_ASSERT_EQUAL(BB_LOG_FLUSH_WAIT_TIMEOUT, bb_log_flush_wait_decide(false, 999, 5));
}

void test_bb_log_flush_wait_decide_own_marker_is_done(void) {
    TEST_ASSERT_EQUAL(BB_LOG_FLUSH_WAIT_DONE, bb_log_flush_wait_decide(true, 5, 5));
}

void test_bb_log_flush_wait_decide_stale_give_is_retry(void) {
    // The exact bug this replaces: a take succeeded, but the completed seq
    // belongs to an earlier caller's abandoned marker (completed_seq < my
    // target_seq) -- must RETRY, never report done.
    TEST_ASSERT_EQUAL(BB_LOG_FLUSH_WAIT_RETRY, bb_log_flush_wait_decide(true, 3, 5));
}

void test_bb_log_flush_wait_decide_ahead_give_is_done(void) {
    TEST_ASSERT_EQUAL(BB_LOG_FLUSH_WAIT_DONE, bb_log_flush_wait_decide(true, 9, 5));
}

// ---------------------------------------------------------------------------
// bb_log_flush_remaining_ms — bounded-budget arithmetic
// ---------------------------------------------------------------------------

void test_bb_log_flush_remaining_ms_partial_elapsed(void) {
    TEST_ASSERT_EQUAL_UINT32(400, bb_log_flush_remaining_ms(1000, 600));
}

void test_bb_log_flush_remaining_ms_zero_elapsed(void) {
    TEST_ASSERT_EQUAL_UINT32(1000, bb_log_flush_remaining_ms(1000, 0));
}

void test_bb_log_flush_remaining_ms_exact_boundary_is_zero(void) {
    TEST_ASSERT_EQUAL_UINT32(0, bb_log_flush_remaining_ms(1000, 1000));
}

void test_bb_log_flush_remaining_ms_overrun_clamps_to_zero(void) {
    // elapsed_ms > budget_ms must clamp to 0, never underflow-wrap.
    TEST_ASSERT_EQUAL_UINT32(0, bb_log_flush_remaining_ms(1000, 5000));
}
