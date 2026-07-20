/* test_bb_mem_arena.c — host tests for the generic bb_mem_arena primitive.
 *
 * Covers all three init variants (caller buffer / heap / spiram-or-host-
 * equivalent), alloc->owns->reset, free_bytes accounting, exhaustion,
 * destroy freeing owned buffers (via bb_mem_set_alloc_hook fail injection
 * for the NO_MEM branch), and every NULL/invalid-arg guard.
 */
#include "unity.h"
#include "bb_mem_arena.h"
#include "bb_mem_test.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARENA_BUF_BYTES 256

static uint8_t s_buf[ARENA_BUF_BYTES] __attribute__((aligned(_Alignof(max_align_t))));

void test_bb_mem_arena_init_from_buffer_succeeds(void)
{
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init(&a, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(bb_mem_arena_free_bytes(a) > 0);
    bb_mem_arena_destroy(a); /* no-op for caller-supplied buffer */
}

void test_bb_mem_arena_init_null_out_returns_invalid_arg(void)
{
    bb_err_t err = bb_mem_arena_init(NULL, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mem_arena_init_null_buf_returns_invalid_arg(void)
{
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init(&a, NULL, sizeof(s_buf));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mem_arena_init_too_small_returns_invalid_arg(void)
{
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init(&a, s_buf, 4);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mem_arena_alloc_owns_reset_roundtrip(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    void *p = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_mem_arena_owns(a, p));

    size_t before_reset = bb_mem_arena_free_bytes(a);
    bb_mem_arena_reset(a);
    TEST_ASSERT_TRUE(bb_mem_arena_free_bytes(a) > before_reset);

    /* Reused space allocates from the start again. */
    void *p2 = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_EQUAL_PTR(p, p2);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_free_bytes_accounting(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t start = bb_mem_arena_free_bytes(a);
    void *p = bb_mem_arena_alloc(a, 32);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(start - 32, bb_mem_arena_free_bytes(a));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_exhaustion_returns_null(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_mem_arena_free_bytes(a);
    void *p = bb_mem_arena_alloc(a, free_before + 8);
    TEST_ASSERT_NULL(p);

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.alloc_failed);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_zero_bytes_returns_null(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    TEST_ASSERT_NULL(bb_mem_arena_alloc(a, 0));
    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_null_arena_returns_null(void)
{
    TEST_ASSERT_NULL(bb_mem_arena_alloc(NULL, 16));
}

void test_bb_mem_arena_free_null_arena_is_noop(void)
{
    bb_mem_arena_free(NULL, s_buf); /* must not crash */
}

void test_bb_mem_arena_free_null_ptr_is_noop(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_mem_arena_free(a, NULL); /* must not crash */
    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_free_updates_stats(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    void *p = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    bb_mem_arena_free(a, p);

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.free_count);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_reset_null_arena_is_noop(void)
{
    bb_mem_arena_reset(NULL); /* must not crash */
}

void test_bb_mem_arena_owns_null_arena_is_false(void)
{
    TEST_ASSERT_FALSE(bb_mem_arena_owns(NULL, s_buf));
}

void test_bb_mem_arena_owns_null_ptr_is_false(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    TEST_ASSERT_FALSE(bb_mem_arena_owns(a, NULL));
    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_owns_out_of_range_ptr_is_false(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    int outside;
    TEST_ASSERT_FALSE(bb_mem_arena_owns(a, &outside));
    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_owns_ptr_before_buf_is_false(void)
{
    /* `a` (the arena header) lives at the start of the backing block, before
     * a->buf (buf = block + BB_MEM_ARENA_HDR_SZ) -- so (const void *)a is a
     * non-NULL pointer guaranteed to be < a->buf, exercising the `ptr >=
     * buf` short-circuit-false branch deterministically (unlike a stack
     * local, whose address relative to a static buffer is layout-dependent). */
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    TEST_ASSERT_FALSE(bb_mem_arena_owns(a, (const void *)a));
    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_owns_ptr_at_end_boundary_is_false(void)
{
    /* One-past-the-last-valid-byte: ptr >= buf is true, but ptr < buf+size
     * is false -- exercises the second half of the && short-circuit that
     * test_bb_mem_arena_owns_out_of_range_ptr_is_false does not
     * deterministically hit. */
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t total = bb_mem_arena_free_bytes(a);
    void *p = bb_mem_arena_alloc(a, total);
    TEST_ASSERT_NOT_NULL(p);

    const uint8_t *end = (const uint8_t *)p + total;
    TEST_ASSERT_FALSE(bb_mem_arena_owns(a, end));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_free_bytes_null_arena_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_free_bytes(NULL));
}

void test_bb_mem_arena_get_stats_null_args_is_noop(void)
{
    bb_mem_arena_get_stats(NULL, NULL); /* must not crash */
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_mem_arena_get_stats(a, NULL); /* must not crash */
    bb_mem_arena_get_stats(NULL, NULL);
    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_destroy_null_is_noop(void)
{
    bb_mem_arena_destroy(NULL); /* must not crash */
}

// ---------------------------------------------------------------------------
// bb_mem-backed variants (init_heap / init_spiram)
// ---------------------------------------------------------------------------

void test_bb_mem_arena_init_heap_succeeds_and_destroy_frees(void)
{
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init_heap(&a, 128);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(a);

    void *p = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_mem_arena_owns(a, p));

    bb_mem_arena_destroy(a); /* frees the bb_mem-backed block */
}

void test_bb_mem_arena_init_spiram_succeeds_and_destroy_frees(void)
{
    /* Host has no real SPIRAM; bb_mem routes this through the same
     * plain-calloc path as init_heap — this is the "host-equivalent" of the
     * SPIRAM-preferred ESP-IDF path. */
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init_spiram(&a, 128);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(a);

    void *p = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_mem_arena_owns(a, p));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_init_heap_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_mem_arena_init_heap(NULL, 128));
}

void test_bb_mem_arena_init_heap_zero_size_returns_invalid_arg(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_mem_arena_init_heap(&a, 0));
}

void test_bb_mem_arena_init_heap_size_too_small_for_carve_returns_invalid_arg(void)
{
    /* size=1 passes the size==0 / overflow checks in
     * bb_mem_arena_init_from_bb_mem, so bb_calloc_prefer_spiram succeeds and
     * allocates BB_MEM_ARENA_HDR_SZ+1 bytes -- but that total is too small
     * for bb_mem_arena_carve's own HDR_SZ+ALIGN floor, so carve fails and
     * the caller must free the just-allocated block before returning the
     * error (exercises the err != BB_OK cleanup branch). */
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init_heap(&a, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mem_arena_init_spiram_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_mem_arena_init_spiram(NULL, 128));
}

void test_bb_mem_arena_init_spiram_zero_size_returns_invalid_arg(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_mem_arena_init_spiram(&a, 0));
}

static void *failing_alloc(size_t sz)
{
    (void)sz;
    return NULL;
}

void test_bb_mem_arena_init_heap_alloc_failure_returns_no_mem(void)
{
    bb_mem_set_alloc_hook(failing_alloc);
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init_heap(&a, 128);
    bb_mem_set_alloc_hook(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_MEM, err);
}

void test_bb_mem_arena_init_spiram_alloc_failure_returns_no_mem(void)
{
    bb_mem_set_alloc_hook(failing_alloc);
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init_spiram(&a, 128);
    bb_mem_set_alloc_hook(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_MEM, err);
}

void test_bb_mem_arena_alloc_huge_bytes_returns_no_space_not_bogus_ptr(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    /* Near-SIZE_MAX request must not wrap the alignment round-up or the
     * offset+aligned bounds check into a bogus success. */
    void *p = bb_mem_arena_alloc(a, SIZE_MAX - 3u);
    TEST_ASSERT_NULL(p);

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.alloc_failed);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_init_heap_size_overflow_returns_invalid_arg(void)
{
    bb_mem_arena_t a = NULL;
    bb_err_t err = bb_mem_arena_init_heap(&a, SIZE_MAX);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mem_arena_double_destroy_caller_buffer_is_safe(void)
{
    /* Only the caller-supplied-buffer path (owned_block == NULL) is safe to
     * double-destroy: nothing is freed, so the struct is never dereferenced
     * after being invalidated. A bb_mem-backed arena (init_heap/init_spiram)
     * must NOT be double-destroyed — its header lives inside the freed
     * block, so a second destroy() is a UAF/double-free (undefined
     * behavior), not a guarantee this library provides. See bb_mem_arena.h. */
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_mem_arena_destroy(a);
    bb_mem_arena_destroy(a); /* must not crash */
}

void test_bb_mem_arena_alloc_returns_max_align_t_aligned_pointer(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    void *p1 = bb_mem_arena_alloc(a, 1);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)p1 % _Alignof(max_align_t));

    void *p2 = bb_mem_arena_alloc(a, 3);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)p2 % _Alignof(max_align_t));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_exact_fit_succeeds(void)
{
    /* free_bytes is already BB_MEM_ARENA_ALIGN-aligned (a fresh arena's data
     * region is sized from BB_MEM_ARENA_HDR_SZ, itself alignment-rounded), so
     * requesting exactly free_bytes must succeed and drain the arena to
     * zero -- the overflow guard's `bytes > remaining` check must not
     * over-reject this boundary case. */
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_mem_arena_free_bytes(a);
    TEST_ASSERT_EQUAL_UINT(0, free_before % _Alignof(max_align_t));

    void *p = bb_mem_arena_alloc(a, free_before);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_free_bytes(a));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// bb_mem_arena_size() — total data-region size accessor.
// ---------------------------------------------------------------------------

void test_bb_mem_arena_size_returns_total_bytes(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t total = bb_mem_arena_size(a);
    size_t free_before = bb_mem_arena_free_bytes(a);
    TEST_ASSERT_EQUAL_UINT(free_before, total); // nothing allocated yet

    void *p = bb_mem_arena_alloc(a, 32);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(total, bb_mem_arena_size(a)); // size is constant
    TEST_ASSERT_EQUAL_UINT(total - 32, bb_mem_arena_free_bytes(a));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_size_null_arena_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_size(NULL));
}

// ---------------------------------------------------------------------------
// bb_mem_arena_stats_t.peak_offset — soak high-watermark, survives reset.
// ---------------------------------------------------------------------------

void test_bb_mem_arena_peak_offset_monotonic_across_reset(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.peak_offset);

    void *p1 = bb_mem_arena_alloc(a, 96);
    TEST_ASSERT_NOT_NULL(p1);
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(96, stats.peak_offset);

    bb_mem_arena_reset(a);
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(96, stats.peak_offset); // reset does not touch peak_offset

    void *p2 = bb_mem_arena_alloc(a, 16); // smaller than the prior high-water mark
    TEST_ASSERT_NOT_NULL(p2);
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(96, stats.peak_offset); // still the pre-reset high

    void *p3 = bb_mem_arena_alloc(a, 32); // 16 + 32 = 48, still under the prior peak
    TEST_ASSERT_NOT_NULL(p3);
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(96, stats.peak_offset);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_peak_offset_advances_past_prior_high(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    void *p1 = bb_mem_arena_alloc(a, 32);
    TEST_ASSERT_NOT_NULL(p1);
    void *p2 = bb_mem_arena_alloc(a, 64); /* cumulative offset 96 > prior 32 */
    TEST_ASSERT_NOT_NULL(p2);

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(96, stats.peak_offset);

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// bb_mem_arena_alloc_rest() -- allocate-the-remainder, rounded DOWN to
// alignment (never up, unlike bb_mem_arena_alloc()). Added for B1-1030
// review: a caller carving a secondary region out of "whatever's left"
// must never spuriously fail just because the leftover byte count isn't
// already alignment-clean.
// ---------------------------------------------------------------------------

void test_bb_mem_arena_alloc_rest_claims_all_remaining_aligned_bytes(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_mem_arena_free_bytes(a);
    TEST_ASSERT_EQUAL_UINT(0, free_before % _Alignof(max_align_t));

    size_t got_size = 0;
    void  *p = bb_mem_arena_alloc_rest(a, &got_size);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(free_before, got_size);   // already aligned -- nothing left behind
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_free_bytes(a));
    TEST_ASSERT_TRUE(bb_mem_arena_owns(a, p));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_rest_rounds_down_on_misaligned_remainder(void)
{
    // A backing buffer whose TOTAL size is deliberately NOT an
    // _Alignof(max_align_t) multiple (unlike s_buf above, which is exactly
    // 256 bytes and alignment-clean) -- the arena's own data-region size
    // (block_size minus the alignment-rounded header) inherits that same
    // misalignment, so bb_mem_arena_free_bytes() is non-aligned here even
    // with NOTHING else allocated yet. This is the exact scenario the naive
    // `bb_mem_arena_alloc(a, bb_mem_arena_free_bytes(a))` form (pre-fix
    // bb_serialize_json_parse_bytes()) got wrong: it rounds that odd
    // remainder UP past what's truly left and returns NULL. alloc_rest()
    // must instead round DOWN and still succeed, claiming a FEW bytes less
    // than the odd free_bytes() count.
    static uint8_t misaligned_buf[256 + 3] __attribute__((aligned(_Alignof(max_align_t))));

    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, misaligned_buf, sizeof(misaligned_buf)));

    size_t free_before = bb_mem_arena_free_bytes(a);
    TEST_ASSERT_NOT_EQUAL_UINT(0, free_before % _Alignof(max_align_t));  // genuinely misaligned

    size_t got_size = 0;
    void  *rest = bb_mem_arena_alloc_rest(a, &got_size);
    TEST_ASSERT_NOT_NULL(rest);
    TEST_ASSERT_EQUAL_UINT(0, got_size % _Alignof(max_align_t));  // rounded DOWN to alignment
    TEST_ASSERT_TRUE(got_size < free_before);                     // strictly less -- the odd tail was dropped
    TEST_ASSERT_EQUAL_UINT(free_before - got_size, bb_mem_arena_free_bytes(a));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_rest_exhausted_returns_null_and_zero(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_mem_arena_free_bytes(a);
    void *drained = bb_mem_arena_alloc(a, free_before);  // exact fit, already aligned
    TEST_ASSERT_NOT_NULL(drained);
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_free_bytes(a));

    size_t got_size = 0xDEADBEEFu;
    void  *rest = bb_mem_arena_alloc_rest(a, &got_size);
    TEST_ASSERT_NULL(rest);
    TEST_ASSERT_EQUAL_UINT(0, got_size);

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.alloc_failed);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_rest_null_arena_returns_null_and_zero(void)
{
    size_t got_size = 0xDEADBEEFu;
    void  *rest = bb_mem_arena_alloc_rest(NULL, &got_size);
    TEST_ASSERT_NULL(rest);
    TEST_ASSERT_EQUAL_UINT(0, got_size);
}

void test_bb_mem_arena_alloc_rest_null_arena_and_null_out_size_is_safe(void)
{
    // Covers the `!a` early-return's own `if (out_size)` arm when out_size
    // is ALSO NULL -- test_..._null_arena_returns_null_and_zero above only
    // exercises the non-NULL out_size arm.
    void *rest = bb_mem_arena_alloc_rest(NULL, NULL);
    TEST_ASSERT_NULL(rest);
}

void test_bb_mem_arena_alloc_rest_null_out_size_is_safe(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    void *rest = bb_mem_arena_alloc_rest(a, NULL);
    TEST_ASSERT_NOT_NULL(rest);
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_free_bytes(a));

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_rest_exhausted_with_null_out_size_is_safe(void)
{
    // Covers the exhausted (`aligned_down == 0`) path's own `if (out_size)`
    // arm when out_size is NULL -- ..._exhausted_returns_null_and_zero above
    // only exercises the non-NULL out_size arm.
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t free_before = bb_mem_arena_free_bytes(a);
    void  *p = bb_mem_arena_alloc(a, free_before);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(0, bb_mem_arena_free_bytes(a));

    void *rest = bb_mem_arena_alloc_rest(a, NULL);
    TEST_ASSERT_NULL(rest);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_rest_does_not_re_raise_peak_offset_below_prior_high_water(void)
{
    // Covers the `a->offset > a->stats.peak_offset` FALSE arm: reset()
    // rewinds offset but leaves peak_offset at its prior high-water mark, so
    // a second alloc_rest() over the same (unchanged) capacity bumps offset
    // back up to but never past that already-recorded peak.
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t total = bb_mem_arena_free_bytes(a);
    size_t first_size = 0;
    TEST_ASSERT_NOT_NULL(bb_mem_arena_alloc_rest(a, &first_size));
    TEST_ASSERT_EQUAL_UINT(total, first_size);

    bb_mem_arena_stats_t stats_after_first;
    bb_mem_arena_get_stats(a, &stats_after_first);
    TEST_ASSERT_EQUAL_UINT(total, stats_after_first.peak_offset);

    bb_mem_arena_reset(a);

    size_t second_size = 0;
    TEST_ASSERT_NOT_NULL(bb_mem_arena_alloc_rest(a, &second_size));
    TEST_ASSERT_EQUAL_UINT(total, second_size);

    bb_mem_arena_stats_t stats_after_second;
    bb_mem_arena_get_stats(a, &stats_after_second);
    TEST_ASSERT_EQUAL_UINT(total, stats_after_second.peak_offset); // unchanged -- not re-raised

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_alloc_rest_updates_stats_and_peak_offset(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    size_t total = bb_mem_arena_free_bytes(a);
    size_t got_size = 0;
    void  *p = bb_mem_arena_alloc_rest(a, &got_size);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(total, got_size);

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.alloc_count);
    TEST_ASSERT_EQUAL_UINT(total, stats.peak_offset);

    bb_mem_arena_destroy(a);
}

void test_bb_mem_arena_destroy_caller_buffer_does_not_touch_bb_mem(void)
{
    /* Destroying a caller-supplied-buffer arena must be a no-op: it must
     * not attempt to free s_buf via bb_mem_free (which would corrupt the
     * static buffer's allocator bookkeeping / crash on a non-heap ptr). */
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_mem_arena_destroy(a);
    /* s_buf must still be usable afterwards for subsequent tests. */
    bb_mem_arena_t b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&b, s_buf, sizeof(s_buf)));
    void *p = bb_mem_arena_alloc(b, 8);
    TEST_ASSERT_NOT_NULL(p);
    bb_mem_arena_destroy(b);
}
