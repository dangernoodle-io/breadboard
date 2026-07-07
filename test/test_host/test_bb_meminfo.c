#include "unity.h"
#include "bb_meminfo.h"

#include <string.h>

void test_bb_meminfo_get_rejects_null(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_meminfo_get(NULL));
}

void test_bb_meminfo_get_host_zeroes_snapshot(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0xAA, sizeof(m));

    TEST_ASSERT_EQUAL(BB_OK, bb_meminfo_get(&m));

    TEST_ASSERT_EQUAL_size_t(0, m.default_region.free);
    TEST_ASSERT_EQUAL_size_t(0, m.default_region.min_ever_free);
    TEST_ASSERT_EQUAL_size_t(0, m.default_region.largest_free_block);
    TEST_ASSERT_EQUAL_size_t(0, m.default_region.total);

    TEST_ASSERT_EQUAL_size_t(0, m.internal.free);
    TEST_ASSERT_EQUAL_size_t(0, m.internal.min_ever_free);
    TEST_ASSERT_EQUAL_size_t(0, m.internal.largest_free_block);
    TEST_ASSERT_EQUAL_size_t(0, m.internal.total);

    TEST_ASSERT_EQUAL_size_t(0, m.dma.free);
    TEST_ASSERT_EQUAL_size_t(0, m.spiram.free);

    TEST_ASSERT_EQUAL_size_t(0, m.esp_min_free_heap);
    TEST_ASSERT_EQUAL_size_t(0, m.mem_outstanding_bytes);
    TEST_ASSERT_EQUAL_size_t(0, m.mem_peak_outstanding);
    TEST_ASSERT_EQUAL_UINT32(0, m.mem_alloc_fail);

    TEST_ASSERT_EQUAL_size_t(0, m.rtc_used);
    TEST_ASSERT_EQUAL_size_t(0, m.rtc_total);
    TEST_ASSERT_EQUAL_size_t(0, m.dram_static_bytes);
}
