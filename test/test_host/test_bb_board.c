#include "unity.h"
#include "bb_board.h"

_Static_assert(sizeof(size_t) >= 4, "size_t must be at least 4 bytes");

void test_bb_board_heap_free_total_callable(void)
{
    size_t sz = bb_board_heap_free_total();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_free_internal_callable(void)
{
    size_t sz = bb_board_heap_free_internal();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_minimum_ever_callable(void)
{
    size_t sz = bb_board_heap_minimum_ever();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_heap_largest_free_block_callable(void)
{
    size_t sz = bb_board_heap_largest_free_block();
    TEST_ASSERT_EQUAL_INT(sz, sz);  // Sanity: no crash, returns size_t
}

void test_bb_board_chip_revision_callable(void)
{
    uint32_t rev = bb_board_chip_revision();
    TEST_ASSERT_EQUAL_UINT32(rev, rev);  // Sanity: no crash, returns uint32_t
}

void test_bb_board_cpu_freq_mhz_callable(void)
{
    uint32_t freq = bb_board_cpu_freq_mhz();
    TEST_ASSERT_EQUAL_UINT32(freq, freq);  // Sanity: no crash, returns uint32_t
}
