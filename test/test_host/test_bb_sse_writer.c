// Host tests for bb_sse_idle pure helper.
// Covers every branch of the idle-accumulation logic used by bb_sse_writer_run.
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

// Include the private helper via the component's src/ directory, which is
// added to the include path by native_scaffold.py for bb_sse_writer.
#include "bb_sse_idle.h"

// ---------------------------------------------------------------------------
// 1. Step below heartbeat: accumulate, no ping
// ---------------------------------------------------------------------------
void test_sse_idle_below_heartbeat(void)
{
    bool should_ping = false;
    uint32_t acc = bb_sse_idle_advance(0, 500, 15000, &should_ping);
    TEST_ASSERT_FALSE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(500, acc);
}

// ---------------------------------------------------------------------------
// 2. Single step exactly equals heartbeat: trigger ping, reset to 0
// ---------------------------------------------------------------------------
void test_sse_idle_exactly_heartbeat(void)
{
    bool should_ping = false;
    uint32_t acc = bb_sse_idle_advance(0, 15000, 15000, &should_ping);
    TEST_ASSERT_TRUE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(0, acc);
}

// ---------------------------------------------------------------------------
// 3. accumulated + step exceeds heartbeat: still triggers ping, resets to 0
// ---------------------------------------------------------------------------
void test_sse_idle_above_heartbeat(void)
{
    bool should_ping = false;
    // Start with 14500 ms already accumulated, step of 1000 → total 15500 ≥ 15000
    uint32_t acc = bb_sse_idle_advance(14500, 1000, 15000, &should_ping);
    TEST_ASSERT_TRUE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(0, acc);
}

// ---------------------------------------------------------------------------
// 4. After ping resets to 0, next call accumulates from scratch
// ---------------------------------------------------------------------------
void test_sse_idle_resets_after_ping(void)
{
    bool should_ping = false;

    // First call: triggers ping
    uint32_t acc = bb_sse_idle_advance(0, 15000, 15000, &should_ping);
    TEST_ASSERT_TRUE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(0, acc);

    // Second call from the reset accumulator: no ping yet
    should_ping = false;
    acc = bb_sse_idle_advance(acc, 500, 15000, &should_ping);
    TEST_ASSERT_FALSE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(500, acc);
}

// ---------------------------------------------------------------------------
// 5. Multiple steps below heartbeat: no ping, value grows
// ---------------------------------------------------------------------------
void test_sse_idle_no_ping_multiple_steps(void)
{
    bool should_ping = false;
    uint32_t acc = 0;

    acc = bb_sse_idle_advance(acc, 500, 15000, &should_ping);
    TEST_ASSERT_FALSE(should_ping);
    acc = bb_sse_idle_advance(acc, 500, 15000, &should_ping);
    TEST_ASSERT_FALSE(should_ping);
    acc = bb_sse_idle_advance(acc, 500, 15000, &should_ping);
    TEST_ASSERT_FALSE(should_ping);

    TEST_ASSERT_EQUAL_UINT32(1500, acc);
}

// ---------------------------------------------------------------------------
// 6. Two steps that together cross heartbeat_ms
// ---------------------------------------------------------------------------
void test_sse_idle_accumulates_across_calls(void)
{
    bool should_ping = false;
    uint32_t acc = 0;

    // First step: 14000 ms — not enough
    acc = bb_sse_idle_advance(acc, 14000, 15000, &should_ping);
    TEST_ASSERT_FALSE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(14000, acc);

    // Second step: 1000 ms more — now 15000 ≥ heartbeat
    acc = bb_sse_idle_advance(acc, 1000, 15000, &should_ping);
    TEST_ASSERT_TRUE(should_ping);
    TEST_ASSERT_EQUAL_UINT32(0, acc);
}

// ---------------------------------------------------------------------------
// 7. Abort-poll slicing: remaining exceeds abort_poll_ms — clamp to poll interval
// ---------------------------------------------------------------------------
void test_sse_abort_poll_slice_clamps_to_poll_interval(void)
{
    uint32_t slice = bb_sse_abort_poll_slice_ms(15000, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000, slice);
}

// ---------------------------------------------------------------------------
// 8. Abort-poll slicing: remaining below abort_poll_ms — use remaining as-is
// ---------------------------------------------------------------------------
void test_sse_abort_poll_slice_uses_remaining_when_smaller(void)
{
    uint32_t slice = bb_sse_abort_poll_slice_ms(400, 1000);
    TEST_ASSERT_EQUAL_UINT32(400, slice);
}

// ---------------------------------------------------------------------------
// 9. Abort-poll slicing: remaining exactly equals abort_poll_ms
// ---------------------------------------------------------------------------
void test_sse_abort_poll_slice_exact_match(void)
{
    uint32_t slice = bb_sse_abort_poll_slice_ms(1000, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000, slice);
}

// ---------------------------------------------------------------------------
// 10. Abort-poll slicing: remaining == 0 — single-shot-timeout semantics
// ---------------------------------------------------------------------------
void test_sse_abort_poll_slice_zero_remaining(void)
{
    uint32_t slice = bb_sse_abort_poll_slice_ms(0, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, slice);
}

// ---------------------------------------------------------------------------
// 11. Abort-poll slicing: summed across calls reproduces the original
// wait_timeout_ms exactly (heartbeat cadence must be unaffected).
// ---------------------------------------------------------------------------
void test_sse_abort_poll_slice_sums_to_wait_timeout(void)
{
    uint32_t wait_timeout_ms = 15000;
    uint32_t abort_poll_ms = 1000;
    uint32_t remaining = wait_timeout_ms;
    uint32_t total = 0;
    int iterations = 0;

    while (remaining > 0) {
        uint32_t slice = bb_sse_abort_poll_slice_ms(remaining, abort_poll_ms);
        TEST_ASSERT_TRUE(slice > 0);
        remaining -= slice;
        total += slice;
        iterations++;
        TEST_ASSERT_TRUE(iterations <= 20);  // guard against infinite loop
    }

    TEST_ASSERT_EQUAL_UINT32(wait_timeout_ms, total);
    TEST_ASSERT_EQUAL_INT(15, iterations);
}
