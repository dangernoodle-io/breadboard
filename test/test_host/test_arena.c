/* test_arena.c — host tests for the generic bb_arena primitive.
 *
 * Covers all three init variants (caller buffer / heap / spiram-or-host-
 * equivalent), alloc->owns->reset, free_bytes accounting, exhaustion,
 * destroy freeing owned buffers (via bb_mem_set_alloc_hook fail injection
 * for the NO_MEM branch), and every NULL/invalid-arg guard.
 */
#include "unity.h"
#include "bb_arena.h"
#include "bb_mem_test.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARENA_BUF_BYTES 256

static uint8_t s_buf[ARENA_BUF_BYTES] __attribute__((aligned(_Alignof(max_align_t))));

void test_arena_init_from_buffer_succeeds(void)
{
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init(&a, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(bb_arena_free_bytes(a) > 0);
    bb_arena_destroy(a); /* no-op for caller-supplied buffer */
}

void test_arena_init_null_out_returns_invalid_arg(void)
{
    bb_err_t err = bb_arena_init(NULL, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_arena_init_null_buf_returns_invalid_arg(void)
{
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init(&a, NULL, sizeof(s_buf));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_arena_init_too_small_returns_invalid_arg(void)
{
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init(&a, s_buf, 4);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_arena_alloc_owns_reset_roundtrip(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));

    void *p = bb_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_arena_owns(a, p));

    size_t before_reset = bb_arena_free_bytes(a);
    bb_arena_reset(a);
    TEST_ASSERT_TRUE(bb_arena_free_bytes(a) > before_reset);

    /* Reused space allocates from the start again. */
    void *p2 = bb_arena_alloc(a, 16);
    TEST_ASSERT_EQUAL_PTR(p, p2);

    bb_arena_destroy(a);
}

void test_arena_free_bytes_accounting(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t start = bb_arena_free_bytes(a);
    void *p = bb_arena_alloc(a, 32);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(start - 32, bb_arena_free_bytes(a));

    bb_arena_destroy(a);
}

void test_arena_alloc_exhaustion_returns_null(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_arena_free_bytes(a);
    void *p = bb_arena_alloc(a, free_before + 8);
    TEST_ASSERT_NULL(p);

    bb_arena_stats_t stats;
    bb_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.alloc_failed);

    bb_arena_destroy(a);
}

void test_arena_alloc_zero_bytes_returns_null(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    TEST_ASSERT_NULL(bb_arena_alloc(a, 0));
    bb_arena_destroy(a);
}

void test_arena_alloc_null_arena_returns_null(void)
{
    TEST_ASSERT_NULL(bb_arena_alloc(NULL, 16));
}

void test_arena_free_null_arena_is_noop(void)
{
    bb_arena_free(NULL, s_buf); /* must not crash */
}

void test_arena_free_null_ptr_is_noop(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_arena_free(a, NULL); /* must not crash */
    bb_arena_destroy(a);
}

void test_arena_free_updates_stats(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    void *p = bb_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    bb_arena_free(a, p);

    bb_arena_stats_t stats;
    bb_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.free_count);

    bb_arena_destroy(a);
}

void test_arena_reset_null_arena_is_noop(void)
{
    bb_arena_reset(NULL); /* must not crash */
}

void test_arena_owns_null_arena_is_false(void)
{
    TEST_ASSERT_FALSE(bb_arena_owns(NULL, s_buf));
}

void test_arena_owns_null_ptr_is_false(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    TEST_ASSERT_FALSE(bb_arena_owns(a, NULL));
    bb_arena_destroy(a);
}

void test_arena_owns_out_of_range_ptr_is_false(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    int outside;
    TEST_ASSERT_FALSE(bb_arena_owns(a, &outside));
    bb_arena_destroy(a);
}

void test_arena_free_bytes_null_arena_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_arena_free_bytes(NULL));
}

void test_arena_get_stats_null_args_is_noop(void)
{
    bb_arena_get_stats(NULL, NULL); /* must not crash */
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_arena_get_stats(a, NULL); /* must not crash */
    bb_arena_get_stats(NULL, NULL);
    bb_arena_destroy(a);
}

void test_arena_destroy_null_is_noop(void)
{
    bb_arena_destroy(NULL); /* must not crash */
}

// ---------------------------------------------------------------------------
// bb_mem-backed variants (init_heap / init_spiram)
// ---------------------------------------------------------------------------

void test_arena_init_heap_succeeds_and_destroy_frees(void)
{
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init_heap(&a, 128);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(a);

    void *p = bb_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_arena_owns(a, p));

    bb_arena_destroy(a); /* frees the bb_mem-backed block */
}

void test_arena_init_spiram_succeeds_and_destroy_frees(void)
{
    /* Host has no real SPIRAM; bb_mem routes this through the same
     * plain-calloc path as init_heap — this is the "host-equivalent" of the
     * SPIRAM-preferred ESP-IDF path. */
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init_spiram(&a, 128);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(a);

    void *p = bb_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_arena_owns(a, p));

    bb_arena_destroy(a);
}

void test_arena_init_heap_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_arena_init_heap(NULL, 128));
}

void test_arena_init_heap_zero_size_returns_invalid_arg(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_arena_init_heap(&a, 0));
}

void test_arena_init_spiram_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_arena_init_spiram(NULL, 128));
}

void test_arena_init_spiram_zero_size_returns_invalid_arg(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_arena_init_spiram(&a, 0));
}

static void *failing_alloc(size_t sz)
{
    (void)sz;
    return NULL;
}

void test_arena_init_heap_alloc_failure_returns_no_mem(void)
{
    bb_mem_set_alloc_hook(failing_alloc);
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init_heap(&a, 128);
    bb_mem_set_alloc_hook(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_MEM, err);
}

void test_arena_init_spiram_alloc_failure_returns_no_mem(void)
{
    bb_mem_set_alloc_hook(failing_alloc);
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init_spiram(&a, 128);
    bb_mem_set_alloc_hook(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_MEM, err);
}

void test_arena_alloc_huge_bytes_returns_no_space_not_bogus_ptr(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));

    /* Near-SIZE_MAX request must not wrap the alignment round-up or the
     * offset+aligned bounds check into a bogus success. */
    void *p = bb_arena_alloc(a, SIZE_MAX - 3u);
    TEST_ASSERT_NULL(p);

    bb_arena_stats_t stats;
    bb_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.alloc_failed);

    bb_arena_destroy(a);
}

void test_arena_init_heap_size_overflow_returns_invalid_arg(void)
{
    bb_arena_t a = NULL;
    bb_err_t err = bb_arena_init_heap(&a, SIZE_MAX);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_arena_double_destroy_caller_buffer_is_safe(void)
{
    /* Only the caller-supplied-buffer path (owned_block == NULL) is safe to
     * double-destroy: nothing is freed, so the struct is never dereferenced
     * after being invalidated. A bb_mem-backed arena (init_heap/init_spiram)
     * must NOT be double-destroyed — its header lives inside the freed
     * block, so a second destroy() is a UAF/double-free (undefined
     * behavior), not a guarantee this library provides. See bb_arena.h. */
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_arena_destroy(a);
    bb_arena_destroy(a); /* must not crash */
}

void test_arena_alloc_returns_max_align_t_aligned_pointer(void)
{
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));

    void *p1 = bb_arena_alloc(a, 1);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)p1 % _Alignof(max_align_t));

    void *p2 = bb_arena_alloc(a, 3);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)p2 % _Alignof(max_align_t));

    bb_arena_destroy(a);
}

void test_arena_alloc_exact_fit_succeeds(void)
{
    /* free_bytes is already BB_ARENA_ALIGN-aligned (a fresh arena's data
     * region is sized from BB_ARENA_HDR_SZ, itself alignment-rounded), so
     * requesting exactly free_bytes must succeed and drain the arena to
     * zero -- the overflow guard's `bytes > remaining` check must not
     * over-reject this boundary case. */
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_arena_free_bytes(a);
    TEST_ASSERT_EQUAL_UINT(0, free_before % _Alignof(max_align_t));

    void *p = bb_arena_alloc(a, free_before);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(0, bb_arena_free_bytes(a));

    bb_arena_destroy(a);
}

void test_arena_destroy_caller_buffer_does_not_touch_bb_mem(void)
{
    /* Destroying a caller-supplied-buffer arena must be a no-op: it must
     * not attempt to free s_buf via bb_mem_free (which would corrupt the
     * static buffer's allocator bookkeeping / crash on a non-heap ptr). */
    bb_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_arena_destroy(a);
    /* s_buf must still be usable afterwards for subsequent tests. */
    bb_arena_t b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_arena_init(&b, s_buf, sizeof(s_buf)));
    void *p = bb_arena_alloc(b, 8);
    TEST_ASSERT_NOT_NULL(p);
    bb_arena_destroy(b);
}
