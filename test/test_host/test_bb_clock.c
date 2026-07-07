#include "unity.h"
#include "bb_clock.h"

// setUp/tearDown: defined in test_main.c (global).

// bb_clock_now_us returns a nonzero monotonic microsecond timestamp.
void test_bb_clock_now_us_nonzero(void)
{
    uint64_t t = bb_clock_now_us();
    TEST_ASSERT_GREATER_THAN_UINT64(0, t);
}

// bb_clock_now_us is monotonic: successive calls never go backwards, and a
// busy loop between calls observes forward progress.
void test_bb_clock_now_us_monotonic_increases(void)
{
    uint64_t a = bb_clock_now_us();
    // Burn a little wall time so the second sample is guaranteed to differ.
    volatile uint32_t spin = 0;
    for (uint32_t i = 0; i < 2000000u; i++) {
        spin += i;
    }
    (void)spin;
    uint64_t b = bb_clock_now_us();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(a, b);
    TEST_ASSERT_GREATER_THAN_UINT64(a, b);
}
