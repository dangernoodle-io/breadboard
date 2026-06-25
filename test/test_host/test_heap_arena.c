/* test_heap_arena.c — host tests for bb_heap_arena routing logic.
 *
 * Built with CONFIG_BB_HEAP_ARENA_BYTES=256 and BB_HEAP_ARENA_TESTING so the
 * arena is enabled and the calloc/free/test_reset hooks are available.
 * 256 bytes gives ~8 arena slots (HDR + 16-byte payload ≈ 32 bytes each)
 * before exhaustion, sufficient to exercise the fallback path.
 */
#include "unity.h"
#include "bb_heap_arena.h"
#include <stdint.h>
#include <stdlib.h>

/* Arena size set by platformio.ini build_flags for the native env. */
#define ARENA_BYTES CONFIG_BB_HEAP_ARENA_BYTES

void test_heap_arena_init_is_idempotent(void)
{
    bb_heap_arena_test_reset();
    bb_heap_arena_init();
    bb_heap_arena_init(); /* second call must not crash */
}

void test_heap_arena_alloc_hit_returns_in_arena(void)
{
    bb_heap_arena_test_reset();
    bb_heap_arena_init();
    void *p = bb_heap_arena_calloc(1, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_heap_arena_owns(p));
    bb_heap_arena_free(p);
}

void test_heap_arena_alloc_is_zeroed(void)
{
    bb_heap_arena_test_reset();
    bb_heap_arena_init();
    uint8_t *p = bb_heap_arena_calloc(1, 32);
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, p[i]);
    }
    bb_heap_arena_free(p);
}

void test_heap_arena_exhausted_falls_back_to_heap(void)
{
    bb_heap_arena_test_reset();
    bb_heap_arena_init();
    /* Exhaust the arena by allocating 16-byte chunks until we get a heap ptr. */
    void *ptrs[64];
    int   n             = 0;
    bool  got_heap_ptr  = false;
    for (int i = 0; i < 64; i++) {
        ptrs[i] = bb_heap_arena_calloc(1, 16);
        if (ptrs[i] == NULL) break;
        n++;
        if (!bb_heap_arena_owns(ptrs[i])) {
            got_heap_ptr = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(got_heap_ptr);
    for (int i = 0; i < n; i++) {
        if (ptrs[i]) bb_heap_arena_free(ptrs[i]);
    }
}

void test_heap_arena_free_in_range_routes_to_arena(void)
{
    bb_heap_arena_test_reset();
    bb_heap_arena_init();
    void *p = bb_heap_arena_calloc(1, 8);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_heap_arena_owns(p));
    bb_heap_arena_free(p); /* must not crash — routes to arena path */
}

void test_heap_arena_free_out_of_range_routes_to_heap(void)
{
    /* Allocate directly from heap (outside arena range). */
    void *p = malloc(8);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_FALSE(bb_heap_arena_owns(p));
    bb_heap_arena_free(p); /* must not crash — routes to free() */
}

void test_heap_arena_free_null_is_noop(void)
{
    bb_heap_arena_free(NULL); /* must not crash */
}

void test_heap_arena_owns_null_is_false(void)
{
    TEST_ASSERT_FALSE(bb_heap_arena_owns(NULL));
}

void test_heap_arena_calloc_zero_returns_null(void)
{
    bb_heap_arena_test_reset();
    bb_heap_arena_init();
    void *p = bb_heap_arena_calloc(0, 16);
    /* our impl returns NULL for total == 0 */
    TEST_ASSERT_NULL(p);
}
