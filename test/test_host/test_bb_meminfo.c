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

void test_bb_meminfo_format_rejects_null(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0, sizeof(m));
    char buf[128];

    TEST_ASSERT_EQUAL(0, bb_meminfo_format(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, bb_meminfo_format(&m, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, bb_meminfo_format(&m, buf, 0));
}

void test_bb_meminfo_format_known_snapshot(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0, sizeof(m));
    m.internal.free               = 111000;
    m.internal.min_ever_free      = 90000;
    m.internal.largest_free_block = 65536;
    m.spiram.free                 = 4000000;
    m.dma.free                    = 32000;
    m.esp_min_free_heap           = 85000;

    char buf[128];
    int n = bb_meminfo_format(&m, buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "heap_int_free=111000 int_min=90000 int_largest=65536 "
        "spiram_free=4000000 dma_free=32000 esp_min_free=85000",
        buf);
}

void test_bb_meminfo_format_truncates_cleanly(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0, sizeof(m));
    m.internal.free = 12345;

    char buf[8];
    int n = bb_meminfo_format(&m, buf, sizeof(buf));

    // snprintf semantics: n is the untruncated length; buf is still
    // NUL-terminated within the given cap.
    TEST_ASSERT_GREATER_THAN(sizeof(buf) - 1, (size_t)n);
    TEST_ASSERT_EQUAL('\0', buf[sizeof(buf) - 1]);
}
