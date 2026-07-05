#include "unity.h"
#include "bb_num.h"

// bb_clampi — clamp x into [lo, hi].

void test_bb_clampi_below_range_returns_lo(void)
{
    TEST_ASSERT_EQUAL_INT32(0, bb_clampi(-5, 0, 10));
}

void test_bb_clampi_above_range_returns_hi(void)
{
    TEST_ASSERT_EQUAL_INT32(10, bb_clampi(15, 0, 10));
}

void test_bb_clampi_in_range_returns_x(void)
{
    TEST_ASSERT_EQUAL_INT32(5, bb_clampi(5, 0, 10));
}

void test_bb_clampi_equal_to_lo_returns_lo(void)
{
    TEST_ASSERT_EQUAL_INT32(0, bb_clampi(0, 0, 10));
}

void test_bb_clampi_equal_to_hi_returns_hi(void)
{
    TEST_ASSERT_EQUAL_INT32(10, bb_clampi(10, 0, 10));
}

// bb_clampf — clamp x into [lo, hi].

void test_bb_clampf_below_range_returns_lo(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, bb_clampf(-5.0f, 0.0f, 10.0f));
}

void test_bb_clampf_above_range_returns_hi(void)
{
    TEST_ASSERT_EQUAL_FLOAT(10.0f, bb_clampf(15.0f, 0.0f, 10.0f));
}

void test_bb_clampf_in_range_returns_x(void)
{
    TEST_ASSERT_EQUAL_FLOAT(5.5f, bb_clampf(5.5f, 0.0f, 10.0f));
}

void test_bb_clampf_equal_to_lo_returns_lo(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, bb_clampf(0.0f, 0.0f, 10.0f));
}

void test_bb_clampf_equal_to_hi_returns_hi(void)
{
    TEST_ASSERT_EQUAL_FLOAT(10.0f, bb_clampf(10.0f, 0.0f, 10.0f));
}
