#include "unity.h"
#include "bb_mem.h"
#include "bb_mem_test.h"
#include <string.h>

#ifdef BB_MEM_TESTING
// Allocator that always returns NULL — used to exercise the alloc_fail path.
static void *s_null_alloc_hook(size_t n) { (void)n; return NULL; }
#endif

// setUp/tearDown: defined in test_main.c (global). bb_mem_reset_stats() is
// called there so stats counters are cleared before every test in the suite.

// ---------------------------------------------------------------------------
// Existing (non-stats) tests — unmodified
// ---------------------------------------------------------------------------

// bb_malloc_prefer_spiram returns a usable (writable) block.
void test_bb_mem_malloc_returns_usable_block(void)
{
    unsigned char *p = bb_malloc_prefer_spiram(64);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xAB, 64);
    TEST_ASSERT_EQUAL_UINT8(0xAB, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, p[63]);
    bb_mem_free(p);
}

// bb_calloc_prefer_spiram zero-initialises the whole block.
void test_bb_mem_calloc_zeroes(void)
{
    unsigned char *p = bb_calloc_prefer_spiram(16, 4); // 64 bytes
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, p[i]);
    }
    bb_mem_free(p);
}

// bb_mem_free(NULL) is a no-op (must not crash).
void test_bb_mem_free_null_is_safe(void)
{
    bb_mem_free(NULL);
    TEST_PASS();
}

// ---------------------------------------------------------------------------
// Stats tests (gated on BB_MEM_STATS_ENABLE — enabled in native build via
// platformio.ini -DBB_MEM_STATS_ENABLE=1).
//
// The counter semantics are:
//   alloc_count  — every call to bb_malloc/bb_calloc, including failures.
//   outstanding  — sum of allocated block sizes currently live.
//   free_count   — every non-NULL bb_mem_free call.
//   peak         — high-water mark of outstanding_bytes.
//   alloc_fail   — calls where the allocator returned NULL (via BB_MEM_TESTING
//                  hook, since the real allocator rarely fails in tests).
// ---------------------------------------------------------------------------

// After one allocation, alloc_count==1, outstanding_bytes >= requested size,
// alloc_fail==0.
void test_bb_mem_stats_alloc_tracks_outstanding(void)
{
    bb_mem_stats_t s;

    void *p = bb_malloc_prefer_spiram(64);
    TEST_ASSERT_NOT_NULL(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.alloc_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.alloc_fail);
    TEST_ASSERT_GREATER_OR_EQUAL(64, s.outstanding_bytes);

    bb_mem_free(p);
}

// After a malloc + free cycle, outstanding_bytes returns to 0 and free_count==1.
void test_bb_mem_stats_free_clears_outstanding(void)
{
    bb_mem_stats_t s;

    void *p = bb_malloc_prefer_spiram(128);
    TEST_ASSERT_NOT_NULL(p);

    bb_mem_free(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, s.free_count);
}

// Peak tracks the high-water mark: alloc A, alloc B, free A →
// peak >= A + B, outstanding == B.
void test_bb_mem_stats_peak_tracks_high_watermark(void)
{
    bb_mem_stats_t s;

    void *a = bb_malloc_prefer_spiram(64);
    void *b = bb_malloc_prefer_spiram(128);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    bb_mem_free(a);

    bb_mem_get_stats(&s);
    // peak must capture the combined high-water mark (A + B simultaneously live)
    TEST_ASSERT_GREATER_OR_EQUAL(64 + 128, s.peak_outstanding);
    // outstanding must now reflect only B
    TEST_ASSERT_GREATER_OR_EQUAL(128, s.outstanding_bytes);

    bb_mem_free(b);
}

// Injected NULL return (via BB_MEM_TESTING hook): alloc_fail increments,
// outstanding_bytes stays at 0.
void test_bb_mem_stats_null_fail_increments_alloc_fail(void)
{
#ifdef BB_MEM_TESTING
    bb_mem_stats_t s;

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    void *p = bb_malloc_prefer_spiram(64);
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_NULL(p);
    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.alloc_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.alloc_fail);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not defined; skip hook test");
#endif
}
