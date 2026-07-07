/* test_pool.c — host tests for bb_pool (B1-478 PR C).
 *
 * Covers all four modes (RETAINED/FIFO/TRANSIENT/SLOTS), both create forms
 * (caller-supplied arena vs owned arena), bb_pool_arena_size_needed
 * correctness + overflow rejection, and SLOTS acquire/exhaust/release/
 * re-acquire alignment.
 */
#include "unity.h"
#include "bb_pool.h"
#include "bb_mem_arena.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define POOL_BUF_BYTES 4096

static uint8_t s_buf[POOL_BUF_BYTES] __attribute__((aligned(_Alignof(max_align_t))));

// ---------------------------------------------------------------------------
// bb_pool_arena_size_needed
// ---------------------------------------------------------------------------

void test_pool_arena_size_needed_null_cfg_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(NULL));
}

void test_pool_arena_size_needed_transient_nonzero(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    TEST_ASSERT_TRUE(bb_pool_arena_size_needed(&cfg) > 0);
}

void test_pool_arena_size_needed_retained_zero_capacity_is_zero(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_RETAINED, .capacity = 0, .max_slot_bytes = 16 };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

void test_pool_arena_size_needed_retained_zero_slot_bytes_is_zero(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_RETAINED, .capacity = 4, .max_slot_bytes = 0 };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

void test_pool_arena_size_needed_unrecognised_mode_is_zero(void)
{
    bb_pool_cfg_t cfg = { .mode = (bb_pool_mode_t)99, .capacity = 4, .max_slot_bytes = 16 };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

void test_pool_arena_size_needed_grows_with_capacity(void)
{
    bb_pool_cfg_t small = { .mode = BB_POOL_MODE_SLOTS, .capacity = 2, .max_slot_bytes = 32 };
    bb_pool_cfg_t big   = { .mode = BB_POOL_MODE_SLOTS, .capacity = 8, .max_slot_bytes = 32 };
    TEST_ASSERT_TRUE(bb_pool_arena_size_needed(&big) > bb_pool_arena_size_needed(&small));
}

void test_pool_arena_size_needed_overflow_capacity_rejected(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = SIZE_MAX, .max_slot_bytes = SIZE_MAX / 2 };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

void test_pool_arena_size_needed_overflow_fifo_header_add_rejected(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_FIFO, .capacity = 2, .max_slot_bytes = SIZE_MAX };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

void test_pool_arena_size_needed_retained_align_up_wrap_overflow(void)
{
    /* max_slot_bytes within BB_POOL_ALIGN-1 of SIZE_MAX must make align_up
     * report overflow (SIZE_MAX sentinel) rather than silently wrapping to a
     * tiny slot_stride. */
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_RETAINED, .capacity = 1, .max_slot_bytes = SIZE_MAX - 1 };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

void test_pool_arena_size_needed_slots_align_up_wrap_overflow(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = 1, .max_slot_bytes = SIZE_MAX - 1 };
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_arena_size_needed(&cfg));
}

// ---------------------------------------------------------------------------
// TRANSIENT mode
// ---------------------------------------------------------------------------

void test_pool_transient_alloc_free_reset(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT, .name = "trans" };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *m1 = bb_pool_alloc(p, 32);
    TEST_ASSERT_NOT_NULL(m1);

    bb_pool_free(p, m1); /* bump allocator: does not reclaim, just accounting */

    bb_mem_arena_stats_t stats;
    bb_pool_get_stats(p, &stats);
    TEST_ASSERT_TRUE(stats.alloc_count >= 1);

    bb_pool_reset(p);
    void *m2 = bb_pool_alloc(p, 32);
    TEST_ASSERT_NOT_NULL(m2);

    bb_pool_destroy(p); /* caller-owned arena: no-op */
    bb_mem_arena_destroy(a);
}

void test_pool_transient_null_pool_is_safe(void)
{
    TEST_ASSERT_NULL(bb_pool_alloc(NULL, 16));
    bb_pool_free(NULL, NULL);
    bb_pool_reset(NULL);
    bb_pool_get_stats(NULL, NULL);
    bb_pool_destroy(NULL);
}

void test_pool_transient_owned_without_reserve_has_zero_capacity(void)
{
    /* Documented pre-fix behaviour without transient_reserve_bytes: an owned
     * TRANSIENT arena carries no bump headroom beyond the pool struct
     * itself, so every bb_pool_alloc() returns NULL. */
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));
    TEST_ASSERT_NULL(bb_pool_alloc(p, 1));
    bb_pool_destroy(p);
}

void test_pool_transient_owned_with_reserve_bytes_usable(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT, .transient_reserve_bytes = 64, .name = "trans-owned" };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));
    TEST_ASSERT_NOT_NULL(p);

    void *m1 = bb_pool_alloc(p, 32);
    TEST_ASSERT_NOT_NULL(m1);
    void *m2 = bb_pool_alloc(p, 32);
    TEST_ASSERT_NOT_NULL(m2);

    /* the 64-byte reserve is now exhausted */
    TEST_ASSERT_NULL(bb_pool_alloc(p, 1));

    bb_pool_reset(p);
    void *m3 = bb_pool_alloc(p, 32);
    TEST_ASSERT_NOT_NULL(m3);

    bb_pool_destroy(p); /* frees the owned arena */
}

// ---------------------------------------------------------------------------
// RETAINED mode
// ---------------------------------------------------------------------------

void test_pool_retained_update_and_get_recycle(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_RETAINED,
        .capacity = 4,
        .max_slot_bytes = 16,
        .name = "retained",
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));

    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    const void *out_ptr = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_pool_retained_get(p, 0, &out_ptr, &out_len));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_retained_update(p, 0, "hello", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_retained_get(p, 0, &out_ptr, &out_len));
    TEST_ASSERT_EQUAL_UINT(5, out_len);
    TEST_ASSERT_EQUAL_MEMORY("hello", out_ptr, 5);

    /* recycle-on-update: no allocation happens on the second write */
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_retained_update(p, 0, "hi", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_retained_get(p, 0, &out_ptr, &out_len));
    TEST_ASSERT_EQUAL_UINT(2, out_len);
    TEST_ASSERT_EQUAL_MEMORY("hi", out_ptr, 2);

    /* other slots remain independent */
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_pool_retained_get(p, 1, &out_ptr, &out_len));

    bb_mem_arena_destroy(a);
}

void test_pool_retained_update_out_of_range_slot(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_RETAINED, .capacity = 2, .max_slot_bytes = 8 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_retained_update(p, 5, "x", 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_retained_update(p, 0, "toolong!!", 9));

    const void *ptr; size_t len;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_retained_get(p, 5, &ptr, &len));

    bb_mem_arena_destroy(a);
}

void test_pool_retained_ops_on_non_retained_pool_return_invalid_state(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_retained_update(p, 0, "x", 1));
    const void *ptr; size_t len;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_retained_get(p, 0, &ptr, &len));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// FIFO mode
// ---------------------------------------------------------------------------

void test_pool_fifo_push_peek_pop_order(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_FIFO,
        .capacity = 3,
        .max_slot_bytes = 8,
        .full_policy = BB_POOL_FULL_REJECT_NEW,
        .name = "fifo",
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "one", 3, 100, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "two", 3, 200, 2));
    TEST_ASSERT_EQUAL_UINT(2, bb_pool_count(p));

    char buf[8];
    size_t len; int64_t ts; uint32_t id;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_peek_oldest(p, buf, sizeof(buf), &len, &ts, &id));
    TEST_ASSERT_EQUAL_UINT(3, len);
    TEST_ASSERT_EQUAL_MEMORY("one", buf, 3);
    TEST_ASSERT_EQUAL_INT64(100, ts);
    TEST_ASSERT_EQUAL_UINT32(1, id);

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_pop_oldest(p));
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_count(p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_peek_oldest(p, buf, sizeof(buf), &len, &ts, &id));
    TEST_ASSERT_EQUAL_MEMORY("two", buf, 3);

    bb_mem_arena_destroy(a);
}

void test_pool_fifo_reject_new_when_full(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_FIFO,
        .capacity = 2,
        .max_slot_bytes = 4,
        .full_policy = BB_POOL_FULL_REJECT_NEW,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "a", 1, 1, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "b", 1, 2, 2));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pool_push(p, "c", 1, 3, 3));
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_dropped(p));
    TEST_ASSERT_EQUAL_UINT(2, bb_pool_count(p));

    /* oldest entry ("a") preserved */
    char buf[4]; size_t len; int64_t ts; uint32_t id;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_peek_oldest(p, buf, sizeof(buf), &len, &ts, &id));
    TEST_ASSERT_EQUAL_MEMORY("a", buf, 1);

    bb_mem_arena_destroy(a);
}

void test_pool_fifo_evict_oldest_when_full(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_FIFO,
        .capacity = 2,
        .max_slot_bytes = 4,
        .full_policy = BB_POOL_FULL_EVICT_OLDEST,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "a", 1, 1, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "b", 1, 2, 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_push(p, "c", 1, 3, 3));
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_dropped(p));
    TEST_ASSERT_EQUAL_UINT(2, bb_pool_count(p));

    /* "a" was evicted; oldest is now "b" */
    char buf[4]; size_t len; int64_t ts; uint32_t id;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_peek_oldest(p, buf, sizeof(buf), &len, &ts, &id));
    TEST_ASSERT_EQUAL_MEMORY("b", buf, 1);

    bb_mem_arena_destroy(a);
}

void test_pool_fifo_peek_pop_empty_returns_not_found(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_FIFO, .capacity = 2, .max_slot_bytes = 4 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    char buf[4]; size_t len; int64_t ts; uint32_t id;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_pool_peek_oldest(p, buf, sizeof(buf), &len, &ts, &id));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_pool_pop_oldest(p));

    bb_mem_arena_destroy(a);
}

void test_pool_fifo_push_oversized_returns_invalid_arg(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_FIFO, .capacity = 2, .max_slot_bytes = 4 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_push(p, "toolong", 7, 1, 1));

    bb_mem_arena_destroy(a);
}

void test_pool_fifo_count_dropped_on_non_fifo_pool_are_zero(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL_UINT(0, bb_pool_count(p));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_dropped(p));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_push(p, "x", 1, 0, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_pop_oldest(p));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// SLOTS mode
// ---------------------------------------------------------------------------

void test_pool_slots_acquire_exhaust_release_reacquire(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 3,
        .max_slot_bytes = sizeof(int),
        .name = "slots",
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    void *s3 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_NOT_NULL(s3);
    TEST_ASSERT_TRUE(s1 != s2 && s2 != s3 && s1 != s3);

    /* every acquired slot is max_align_t-aligned */
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)s1 % _Alignof(max_align_t));
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)s2 % _Alignof(max_align_t));
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)s3 % _Alignof(max_align_t));

    /* exhausted */
    TEST_ASSERT_NULL(bb_pool_acquire(p));

    /* release then re-acquire returns a usable, previously-released slot */
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s2));
    void *s4 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_PTR(s2, s4);

    *(int *)s4 = 42;
    TEST_ASSERT_EQUAL_INT(42, *(int *)s4);

    bb_mem_arena_destroy(a);
}

void test_pool_slots_release_foreign_pointer_returns_invalid_arg(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = 2, .max_slot_bytes = 8 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    int outside;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_release(p, &outside));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_release(p, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_release(NULL, &outside));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_double_release_returns_invalid_state(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = 2, .max_slot_bytes = 8 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    /* second release of the same still-in-bounds pointer must be rejected,
     * not silently double-pushed onto the free-list */
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_release(p, s1));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_release_one_twice_does_not_corrupt_free_list(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = 3, .max_slot_bytes = sizeof(int) };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    void *s3 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_NOT_NULL(s3);

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s2));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_release(p, s2));

    /* exactly one slot (s2) is free; a subsequent acquire returns that
     * single distinct slot and the pool is exhausted again immediately
     * after — proving the double-release attempt did not push s2 twice */
    void *s4 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_PTR(s2, s4);
    TEST_ASSERT_NULL(bb_pool_acquire(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_storage_is_zero_initialized(void)
{
    /* Fill the backing buffer with a non-zero pattern before create() so a
     * pass just reflects leftover arena content instead of accidental
     * zeroing elsewhere. */
    memset(s_buf, 0xA5, sizeof(s_buf));

    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = 2, .max_slot_bytes = sizeof(int) * 4 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);

    static const uint8_t zeros[sizeof(int) * 4] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zeros, s1, sizeof(zeros));
    TEST_ASSERT_EQUAL_MEMORY(zeros, s2, sizeof(zeros));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_ops_on_non_slots_pool(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_NULL(bb_pool_acquire(p));
    TEST_ASSERT_NULL(bb_pool_acquire(NULL));
    int dummy;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_release(p, &dummy));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// bb_pool_create argument validation
// ---------------------------------------------------------------------------

void test_pool_create_null_args_return_invalid_arg(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create(NULL, a, &p));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create(&cfg, NULL, &p));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create(&cfg, a, NULL));

    bb_mem_arena_destroy(a);
}

void test_pool_create_invalid_mode_returns_invalid_arg(void)
{
    bb_pool_cfg_t cfg = { .mode = (bb_pool_mode_t)77 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create(&cfg, a, &p));

    bb_mem_arena_destroy(a);
}

void test_pool_create_overflow_sizes_returns_invalid_arg_no_oob(void)
{
    /* capacity/max_slot_bytes that would overflow the raw carve
     * multiplications must be rejected up front by bb_pool_create() (via
     * the shared bb_pool_arena_size_needed overflow-checked path), not
     * partially carved into a small backing arena. */
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = SIZE_MAX, .max_slot_bytes = SIZE_MAX / 2 };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create(&cfg, a, &p));

    /* arena is untouched by the rejected create — still usable, evidence
     * that no out-of-bounds carve/write happened before the size check. */
    void *m = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(m);

    bb_mem_arena_destroy(a);
}

void test_pool_create_exhausted_arena_returns_no_space(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_RETAINED, .capacity = 4, .max_slot_bytes = 64 };
    /* Sized to be valid (>= arena header + alignment, currently 64+16=80 on
     * a 64-bit host with max_align_t alignment 16) but far below what a
     * RETAINED pool of capacity=4, max_slot_bytes=64 needs (hundreds of
     * bytes) — so bb_mem_arena_init succeeds but bb_pool_create still reports
     * BB_ERR_NO_SPACE. */
    static uint8_t tiny[96] __attribute__((aligned(_Alignof(max_align_t))));
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, tiny, sizeof(tiny)));
    bb_pool_t p = NULL;

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pool_create(&cfg, a, &p));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// bb_pool_create_owned — HEAP + SPIRAM backing
// ---------------------------------------------------------------------------

void test_pool_create_owned_heap_use_destroy_frees(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 4,
        .max_slot_bytes = 32,
        .name = "owned-heap",
    };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));
    TEST_ASSERT_NOT_NULL(p);

    void *s = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s);

    bb_pool_destroy(p); /* frees the owned arena (and everything in it) */
}

void test_pool_create_owned_spiram_use_destroy_frees(void)
{
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_RETAINED,
        .capacity = 2,
        .max_slot_bytes = 16,
        .name = "owned-spiram",
    };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_SPIRAM, &p));
    TEST_ASSERT_NOT_NULL(p);

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_retained_update(p, 0, "spiram", 6));

    bb_pool_destroy(p);
}

void test_pool_create_owned_exhaustion_via_tiny_capacity_still_works(void)
{
    /* Owned arena is exactly right-sized: a SLOTS pool at its full capacity
     * must exhaust cleanly (NULL on the (capacity+1)th acquire), not
     * silently succeed into overrun. */
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_SLOTS, .capacity = 1, .max_slot_bytes = 8 };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NULL(bb_pool_acquire(p));

    bb_pool_destroy(p);
}

void test_pool_create_owned_null_args_return_invalid_arg(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_pool_t p = NULL;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create_owned(NULL, BB_POOL_BACKING_HEAP, &p));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, NULL));
}

void test_pool_create_owned_invalid_cfg_returns_invalid_arg(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_RETAINED, .capacity = 0, .max_slot_bytes = 0 };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));
}

// ---------------------------------------------------------------------------
// caller-supplied arena: bb_pool_destroy must NOT free it
// ---------------------------------------------------------------------------

void test_pool_destroy_does_not_free_caller_supplied_arena(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    bb_pool_destroy(p); /* must be a no-op on the arena */

    /* arena is still usable after pool destroy */
    void *m = bb_mem_arena_alloc(a, 16);
    TEST_ASSERT_NOT_NULL(m);

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// bb_pool_get_stats
// ---------------------------------------------------------------------------

void test_pool_get_stats_null_args_is_noop(void)
{
    bb_pool_get_stats(NULL, NULL);
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));
    bb_pool_get_stats(p, NULL); /* must not crash */
    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// SLOTS mode — optional per-slot lifecycle callbacks (B1-479 / B1-492)
// ---------------------------------------------------------------------------

typedef struct {
    int  acquire_calls;
    int  release_calls;
    void *last_acquire_slot;
    void *last_release_slot;
    bool  reusable_return;   /* what slot_reusable() should answer next call */
    void *ready_ptr;         /* if set, spy_slot_reusable_selective returns true only for this slot */
    int   reusable_calls;
    int   reap_calls;
    void *last_reap_slot;
} cb_spy_t;

static void spy_on_acquire(void *ctx, void *slot)
{
    cb_spy_t *s = (cb_spy_t *)ctx;
    s->acquire_calls++;
    s->last_acquire_slot = slot;
}

static void spy_on_release(void *ctx, void *slot)
{
    cb_spy_t *s = (cb_spy_t *)ctx;
    s->release_calls++;
    s->last_release_slot = slot;
}

static bool spy_slot_reusable(void *ctx, void *slot)
{
    (void)slot;
    cb_spy_t *s = (cb_spy_t *)ctx;
    s->reusable_calls++;
    return s->reusable_return;
}

static void spy_slot_reap(void *ctx, void *slot)
{
    cb_spy_t *s = (cb_spy_t *)ctx;
    s->reap_calls++;
    s->last_reap_slot = slot;
}

// Differentiates readiness per-slot (by pointer identity) rather than a
// single global flag — needed to exercise bb_pool_slots_reap_ready() reaping
// exactly one of several pending slots while leaving the others pending.
static bool spy_slot_reusable_selective(void *ctx, void *slot)
{
    cb_spy_t *s = (cb_spy_t *)ctx;
    s->reusable_calls++;
    return slot == s->ready_ptr;
}

void test_pool_slots_on_acquire_on_release_fire(void)
{
    cb_spy_t spy = {0};
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 2,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .on_acquire = spy_on_acquire,
        .on_release = spy_on_release,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL_INT(1, spy.acquire_calls);
    TEST_ASSERT_EQUAL_PTR(s1, spy.last_acquire_slot);
    TEST_ASSERT_EQUAL_INT(0, spy.release_calls);

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL_INT(1, spy.release_calls);
    TEST_ASSERT_EQUAL_PTR(s1, spy.last_release_slot);

    /* No slot_reusable configured: released slot is immediately reissuable
     * (today's synchronous behavior), and on_acquire fires again. */
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_PTR(s1, s2);
    TEST_ASSERT_EQUAL_INT(2, spy.acquire_calls);

    bb_mem_arena_destroy(a);
}

void test_pool_slots_not_reusable_blocks_reissue(void)
{
    cb_spy_t spy = {0};
    spy.reusable_return = false;
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable,
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));

    /* Not yet reusable: acquire must return NULL (no reissue), and reap must
     * never be invoked for a slot that hasn't cleared the reusable check. */
    TEST_ASSERT_NULL(bb_pool_acquire(p));
    TEST_ASSERT_TRUE(spy.reusable_calls >= 1);
    TEST_ASSERT_EQUAL_INT(0, spy.reap_calls);

    bb_mem_arena_destroy(a);
}

void test_pool_slots_reusable_true_reaps_then_reissues(void)
{
    cb_spy_t spy = {0};
    spy.reusable_return = false;
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .on_acquire = spy_on_acquire,
        .slot_reusable = spy_slot_reusable,
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    int acquire_calls_after_first = spy.acquire_calls;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));

    /* Still not reusable: acquire fails, no reap. */
    TEST_ASSERT_NULL(bb_pool_acquire(p));
    TEST_ASSERT_EQUAL_INT(0, spy.reap_calls);

    /* Now becomes reusable: acquire reaps once then reissues the same
     * pointer, firing on_acquire again. */
    spy.reusable_return = true;
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_PTR(s1, s2);
    TEST_ASSERT_EQUAL_INT(1, spy.reap_calls);
    TEST_ASSERT_EQUAL_PTR(s1, spy.last_reap_slot);
    TEST_ASSERT_EQUAL_INT(acquire_calls_after_first + 1, spy.acquire_calls);

    bb_mem_arena_destroy(a);
}

void test_pool_slots_pending_slot_ignored_while_free_list_nonempty(void)
{
    /* capacity=3, only s1/s2 ever acquired: slot index 2 is never touched
     * and stays on the free-list from pool creation (bb_pool_create seeds
     * the free-list with every slot up front). Release s1 (with
     * slot_reusable configured, every release goes "pending" — there is no
     * path back onto the free-list once a slot has been acquired at least
     * once). A subsequent acquire must return the untouched free-list slot
     * directly, WITHOUT ever consulting slot_reusable (bb_pool_acquire
     * checks the free-list before scanning pending slots) — proving
     * free-list priority, not just pool exhaustion. */
    cb_spy_t spy = {0};
    spy.reusable_return = false;
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 3,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable,
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1)); /* pending, never reusable */

    int reusable_calls_before = spy.reusable_calls;

    /* free-list still holds the never-acquired third slot — acquire must
     * return it directly without ever calling slot_reusable, even though a
     * pending slot (s1) exists. */
    void *s3 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s3);
    TEST_ASSERT_TRUE(s3 != s1 && s3 != s2);
    TEST_ASSERT_EQUAL_INT(reusable_calls_before, spy.reusable_calls);

    bb_mem_arena_destroy(a);
}

void test_pool_slots_double_release_rejected_with_reusable_configured(void)
{
    cb_spy_t spy = {0};
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_pool_release(p, s1));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// SLOTS mode — idle-reclaim garbage-collection pass (B1-492)
// ---------------------------------------------------------------------------

void test_pool_slots_pending_count_zero_on_fresh_pool(void)
{
    cb_spy_t spy = {0};
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 2,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable_selective,
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_pending_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_pending_count_zero_without_slot_reusable(void)
{
    // No slot_reusable configured: released slots go straight to the
    // free-list, never pending — bb_pool_slots_pending_count() stays 0
    // regardless of how many acquire/release cycles run.
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_pending_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_pending_count_reflects_released_pending_slots(void)
{
    cb_spy_t spy = {0};
    spy.reusable_return = false;
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 2,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable_selective, // ready_ptr NULL -> never ready
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_pending_count(p));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s2));
    TEST_ASSERT_EQUAL_UINT(2, bb_pool_slots_pending_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_reap_ready_moves_ready_slot_to_free_list(void)
{
    cb_spy_t spy = {0};
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 2,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable_selective,
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s2));
    TEST_ASSERT_EQUAL_UINT(2, bb_pool_slots_pending_count(p));

    // Only s1 is "ready" — the reap pass must reap exactly that one, leaving
    // s2 pending untouched (no acquire/reissue happens for either).
    spy.ready_ptr = s1;
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_reap_ready(p));
    TEST_ASSERT_EQUAL_INT(1, spy.reap_calls);
    TEST_ASSERT_EQUAL_PTR(s1, spy.last_reap_slot);
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_pending_count(p));

    // s1 is now back on the free-list — a fresh acquire returns it directly,
    // without ever consulting slot_reusable again (free-list priority).
    int reusable_calls_before = spy.reusable_calls;
    void *s3 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_PTR(s1, s3);
    TEST_ASSERT_EQUAL_INT(reusable_calls_before, spy.reusable_calls);

    bb_mem_arena_destroy(a);
}

void test_pool_slots_reap_ready_leaves_not_ready_slots_pending(void)
{
    cb_spy_t spy = {0};
    spy.reusable_return = false;
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable_selective, // ready_ptr NULL -> never ready
        .slot_reap = spy_slot_reap,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));

    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_reap_ready(p));
    TEST_ASSERT_EQUAL_INT(0, spy.reap_calls);
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_pending_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_reap_ready_noop_without_slot_reusable(void)
{
    // No slot_reusable configured at all: reap_ready must be a pure no-op
    // (0 reaped) even though a slot has been released — there is no pending
    // state to scan.
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_reap_ready(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_reap_ready_and_pending_count_null_pool_is_noop(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_reap_ready(NULL));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_pending_count(NULL));
}

void test_pool_slots_reap_ready_and_pending_count_non_slots_mode_is_noop(void)
{
    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_reap_ready(p));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_pending_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_acquired_count_tracks_loan_state(void)
{
    // No slot_reusable needed here — acquired_count tracks the on-loan
    // bitmap directly, independent of the pending-reap path.
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 2,
        .max_slot_bytes = sizeof(int),
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_acquired_count(p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_acquired_count(p));

    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_UINT(2, bb_pool_slots_acquired_count(p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_acquired_count(p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s2));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_acquired_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_acquired_count_stays_set_while_pending_reap(void)
{
    // With slot_reusable configured and never returning true, a released
    // slot moves from acquired -> pending, never lingering "acquired" — this
    // proves acquired_count and pending_count are mutually exclusive views
    // of the same slot's lifecycle, not double-counting the same state.
    cb_spy_t spy = {0};
    spy.reusable_return = false;
    bb_pool_cfg_t cfg = {
        .mode = BB_POOL_MODE_SLOTS,
        .capacity = 1,
        .max_slot_bytes = sizeof(int),
        .cb_ctx = &spy,
        .slot_reusable = spy_slot_reusable_selective,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_acquired_count(p));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_pending_count(p));

    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1));
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_acquired_count(p));
    TEST_ASSERT_EQUAL_UINT(1, bb_pool_slots_pending_count(p));

    bb_mem_arena_destroy(a);
}

void test_pool_slots_acquired_count_null_and_non_slots_mode_is_noop(void)
{
    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_acquired_count(NULL));

    bb_pool_cfg_t cfg = { .mode = BB_POOL_MODE_TRANSIENT };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    TEST_ASSERT_EQUAL_UINT(0, bb_pool_slots_acquired_count(p));

    bb_mem_arena_destroy(a);
}

// ---------------------------------------------------------------------------
// SLOTS mode — on_destroy per-slot finalizer (firmware-review finding, PR-B)
// ---------------------------------------------------------------------------

static int s_destroy_calls;
static void spy_on_destroy(void *ctx, void *slot)
{
    (void)slot;
    int *n = (int *)ctx;
    (*n)++;
}

void test_pool_slots_on_destroy_fires_once_per_slot_including_never_acquired(void)
{
    // capacity=4, only 2 ever acquired (one released, one left on loan) —
    // on_destroy must still fire for ALL 4 slots: the two never touched, the
    // one released back to the free-list, and the one still acquired.
    s_destroy_calls = 0;
    bb_pool_cfg_t cfg = {
        .mode           = BB_POOL_MODE_SLOTS,
        .capacity       = 4,
        .max_slot_bytes = sizeof(int),
        .cb_ctx         = &s_destroy_calls,
        .on_destroy     = spy_on_destroy,
    };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));

    void *s1 = bb_pool_acquire(p);
    void *s2 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_release(p, s1)); /* s1 -> free-list */
    /* s2 stays on loan; s3/s4 never touched */

    bb_pool_destroy(p); /* frees the owned arena */

    TEST_ASSERT_EQUAL_INT(4, s_destroy_calls);
}

void test_pool_slots_on_destroy_null_is_safe_noop(void)
{
    // No on_destroy configured: bb_pool_destroy must behave exactly as
    // before (backward-compat for every existing SLOTS consumer, e.g.
    // bb_pub's ring pool).
    bb_pool_cfg_t cfg = {
        .mode           = BB_POOL_MODE_SLOTS,
        .capacity       = 2,
        .max_slot_bytes = sizeof(int),
    };
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &p));

    void *s1 = bb_pool_acquire(p);
    TEST_ASSERT_NOT_NULL(s1);

    bb_pool_destroy(p); /* must not crash with on_destroy == NULL */
}

void test_pool_slots_on_destroy_not_called_for_non_slots_mode(void)
{
    // on_destroy is documented as SLOTS-mode only; a TRANSIENT pool's
    // (unused) cfg.on_destroy field must never be invoked.
    s_destroy_calls = 0;
    bb_pool_cfg_t cfg = {
        .mode       = BB_POOL_MODE_TRANSIENT,
        .cb_ctx     = &s_destroy_calls,
        .on_destroy = spy_on_destroy,
    };
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_buf, sizeof(s_buf)));
    bb_pool_t p = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pool_create(&cfg, a, &p));

    bb_pool_destroy(p); /* caller-owned arena: no-op, and no on_destroy call */
    TEST_ASSERT_EQUAL_INT(0, s_destroy_calls);

    bb_mem_arena_destroy(a);
}
