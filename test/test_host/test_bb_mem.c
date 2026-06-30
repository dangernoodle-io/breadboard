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

// ---------------------------------------------------------------------------
// New cap-variant happy-path tests
// ---------------------------------------------------------------------------

// bb_malloc_internal returns a usable block.
void test_bb_mem_malloc_internal_usable(void)
{
    unsigned char *p = bb_malloc_internal(32);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xCD, 32);
    TEST_ASSERT_EQUAL_UINT8(0xCD, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xCD, p[31]);
    bb_mem_free(p);
}

// bb_calloc_internal zero-initialises the block.
void test_bb_mem_calloc_internal_zeroes(void)
{
    unsigned char *p = bb_calloc_internal(8, 4); // 32 bytes
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, p[i]);
    }
    bb_mem_free(p);
}

// bb_malloc_dma returns a usable block on host (no cap constraint).
void test_bb_mem_malloc_dma_usable(void)
{
    unsigned char *p = bb_malloc_dma(64);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xEF, 64);
    TEST_ASSERT_EQUAL_UINT8(0xEF, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, p[63]);
    bb_mem_free(p);
    // Note: the no-fallback DMA contract is enforced on ESP-IDF only; host
    // malloc has no heap-cap concept, so NULL is never returned for DMA here.
}

// bb_realloc_prefer_spiram grows a block and preserves existing content.
void test_bb_mem_realloc_prefer_spiram_grows(void)
{
    unsigned char *p = bb_malloc_prefer_spiram(32);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xAA, 32);

    unsigned char *q = bb_realloc_prefer_spiram(p, 64);
    TEST_ASSERT_NOT_NULL(q);
    // first 32 bytes must be preserved
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, q[i]);
    }
    bb_mem_free(q);
}

// bb_realloc_prefer_spiram shrinks a block.
void test_bb_mem_realloc_prefer_spiram_shrinks(void)
{
    unsigned char *p = bb_malloc_prefer_spiram(128);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xBB, 128);

    unsigned char *q = bb_realloc_prefer_spiram(p, 16);
    TEST_ASSERT_NOT_NULL(q);
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xBB, q[i]);
    }
    bb_mem_free(q);
}

// ---------------------------------------------------------------------------
// New cap-variant stats accounting tests
// ---------------------------------------------------------------------------

// bb_malloc_internal + bb_mem_free: outstanding returns to baseline and
// internal_alloc_count increments.
void test_bb_mem_stats_internal_alloc_cycle(void)
{
#if BB_MEM_STATS_ENABLE
    bb_mem_stats_t s;

    void *p = bb_malloc_internal(64);
    TEST_ASSERT_NOT_NULL(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.alloc_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.internal_alloc_count);
    TEST_ASSERT_GREATER_OR_EQUAL(64, s.outstanding_bytes);

    bb_mem_free(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, s.free_count);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_STATS_ENABLE=0; skip stats test");
#endif
}

// bb_calloc_internal: internal_alloc_count increments.
void test_bb_mem_stats_calloc_internal_counted(void)
{
#if BB_MEM_STATS_ENABLE
    bb_mem_stats_t s;

    void *p = bb_calloc_internal(4, 16); // 64 bytes
    TEST_ASSERT_NOT_NULL(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.internal_alloc_count);
    TEST_ASSERT_GREATER_OR_EQUAL(64, s.internal_alloc_bytes);

    bb_mem_free(p);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_STATS_ENABLE=0; skip stats test");
#endif
}

// bb_malloc_dma + bb_mem_free: outstanding returns to 0 and
// internal_alloc_count increments (host treats DMA as internal).
void test_bb_mem_stats_dma_alloc_cycle(void)
{
#if BB_MEM_STATS_ENABLE
    bb_mem_stats_t s;

    void *p = bb_malloc_dma(64);
    TEST_ASSERT_NOT_NULL(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.internal_alloc_count);
    TEST_ASSERT_GREATER_OR_EQUAL(64, s.outstanding_bytes);

    bb_mem_free(p);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_STATS_ENABLE=0; skip stats test");
#endif
}

// bb_realloc_prefer_spiram grow: after malloc+realloc+free the outstanding
// returns to 0 and alloc_count==2 (malloc + realloc each count).
void test_bb_mem_stats_realloc_grow_accounting(void)
{
#if BB_MEM_STATS_ENABLE
    bb_mem_stats_t s;

    void *p = bb_malloc_prefer_spiram(32);
    TEST_ASSERT_NOT_NULL(p);

    void *q = bb_realloc_prefer_spiram(p, 128);
    TEST_ASSERT_NOT_NULL(q);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(2, s.alloc_count);
    TEST_ASSERT_GREATER_OR_EQUAL(128, s.outstanding_bytes);

    bb_mem_free(q);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_STATS_ENABLE=0; skip stats test");
#endif
}

// bb_realloc_prefer_spiram shrink: outstanding decreases to the smaller size.
void test_bb_mem_stats_realloc_shrink_accounting(void)
{
#if BB_MEM_STATS_ENABLE
    bb_mem_stats_t s;

    void *p = bb_malloc_prefer_spiram(256);
    TEST_ASSERT_NOT_NULL(p);

    bb_mem_get_stats(&s);
    size_t after_alloc = s.outstanding_bytes;
    TEST_ASSERT_GREATER_OR_EQUAL(256, after_alloc);

    void *q = bb_realloc_prefer_spiram(p, 32);
    TEST_ASSERT_NOT_NULL(q);

    bb_mem_get_stats(&s);
    // outstanding should be smaller than after the original alloc
    TEST_ASSERT_LESS_THAN(after_alloc, s.outstanding_bytes);
    TEST_ASSERT_GREATER_OR_EQUAL(32, s.outstanding_bytes);

    bb_mem_free(q);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_STATS_ENABLE=0; skip stats test");
#endif
}

// ---------------------------------------------------------------------------
// Failure-path tests for cap variants using the BB_MEM_TESTING hook
// ---------------------------------------------------------------------------

// bb_malloc_internal with NULL hook: alloc_fail increments, return is NULL.
void test_bb_mem_stats_malloc_internal_null_fail(void)
{
#ifdef BB_MEM_TESTING
    bb_mem_stats_t s;

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    void *p = bb_malloc_internal(64);
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

// bb_calloc_internal with NULL hook: alloc_fail increments, return is NULL.
void test_bb_mem_stats_calloc_internal_null_fail(void)
{
#ifdef BB_MEM_TESTING
    bb_mem_stats_t s;

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    void *p = bb_calloc_internal(4, 16);
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

// bb_malloc_dma with NULL hook: alloc_fail increments, return is NULL.
void test_bb_mem_stats_dma_null_fail(void)
{
#ifdef BB_MEM_TESTING
    bb_mem_stats_t s;

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    void *p = bb_malloc_dma(64);
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

// bb_realloc_prefer_spiram with NULL hook: alloc_fail increments, the original
// pointer remains valid, and outstanding_bytes is unchanged (original block
// is still live; realloc failure must not disturb existing accounting).
void test_bb_mem_stats_realloc_null_fail_outstanding_unchanged(void)
{
#ifdef BB_MEM_TESTING
    bb_mem_stats_t s;

    void *ptr = bb_malloc_prefer_spiram(64);
    TEST_ASSERT_NOT_NULL(ptr);

    bb_mem_get_stats(&s);
    size_t   outstanding_before = s.outstanding_bytes;
    uint32_t alloc_count_before = s.alloc_count;

    bb_mem_set_alloc_hook(s_null_alloc_hook);
    void *q = bb_realloc_prefer_spiram(ptr, 128);
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_NULL(q);
    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(alloc_count_before + 1, s.alloc_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.alloc_fail);
    // original block still live — outstanding must not have changed
    TEST_ASSERT_EQUAL(outstanding_before, s.outstanding_bytes);

    // original ptr is still valid; free it to balance outstanding
    bb_mem_free(ptr);

    bb_mem_get_stats(&s);
    TEST_ASSERT_EQUAL(0, s.outstanding_bytes);
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not defined; skip hook test");
#endif
}
