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
