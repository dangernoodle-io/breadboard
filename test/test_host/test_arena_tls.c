/* test_arena_tls.c — host tests for bb_arena_tls routing logic.
 *
 * Built with CONFIG_BB_ARENA_TLS_BYTES=256 and BB_ARENA_TLS_TESTING so the
 * arena is enabled and the calloc/free/test_reset hooks are available.
 * 256 bytes (minus the bb_arena header) gives a handful of 16-byte-aligned
 * slots before exhaustion, sufficient to exercise the fallback path.
 */
#include "unity.h"
#include "bb_arena_tls.h"
#include <stdint.h>
#include <stdlib.h>

/* Arena size set by platformio.ini build_flags for the native env. */
#define ARENA_BYTES CONFIG_BB_ARENA_TLS_BYTES

void test_arena_tls_init_is_idempotent(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();
    bb_arena_tls_init(); /* second call must not crash */
}

void test_arena_tls_alloc_hit_returns_in_arena(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();
    void *p = bb_arena_tls_calloc(1, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_arena_tls_owns(p));
    bb_arena_tls_free(p);
}

void test_arena_tls_alloc_is_zeroed(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();
    uint8_t *p = bb_arena_tls_calloc(1, 32);
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, p[i]);
    }
    bb_arena_tls_free(p);
}

void test_arena_tls_exhausted_falls_back_to_heap(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();
    /* Exhaust the arena by allocating 16-byte chunks until we get a heap ptr. */
    void *ptrs[64];
    int   n             = 0;
    bool  got_heap_ptr  = false;
    for (int i = 0; i < 64; i++) {
        ptrs[i] = bb_arena_tls_calloc(1, 16);
        if (ptrs[i] == NULL) break;
        n++;
        if (!bb_arena_tls_owns(ptrs[i])) {
            got_heap_ptr = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(got_heap_ptr);
    for (int i = 0; i < n; i++) {
        if (ptrs[i]) bb_arena_tls_free(ptrs[i]);
    }
}

void test_arena_tls_free_in_range_routes_to_arena(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();
    void *p = bb_arena_tls_calloc(1, 8);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_arena_tls_owns(p));
    bb_arena_tls_free(p); /* must not crash — routes to arena path */
}

void test_arena_tls_free_out_of_range_routes_to_heap(void)
{
    /* Allocate directly from heap (outside arena range). */
    void *p = malloc(8);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_FALSE(bb_arena_tls_owns(p));
    bb_arena_tls_free(p); /* must not crash — routes to free() */
}

void test_arena_tls_free_null_is_noop(void)
{
    bb_arena_tls_free(NULL); /* must not crash */
}

void test_arena_tls_owns_null_is_false(void)
{
    TEST_ASSERT_FALSE(bb_arena_tls_owns(NULL));
}

void test_arena_tls_calloc_zero_returns_null(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();
    void *p = bb_arena_tls_calloc(0, 16);
    /* our impl returns NULL for total == 0 */
    TEST_ASSERT_NULL(p);
}

/* Reset-on-drain: once every arena-owned allocation is freed, the arena
 * must be reset (bump pointer rewound) so the NEXT handshake is served
 * arena-first again — a bump allocator that never reclaims would silently
 * fall through to the heap fallback forever after the first handshake. */
void test_arena_tls_reuse_after_free_all(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();

    /* Fill the arena to near-exhaustion with 16-byte chunks. */
    void *ptrs[64];
    int   n = 0;
    for (int i = 0; i < 64; i++) {
        void *p = bb_arena_tls_calloc(1, 16);
        if (p == NULL || !bb_arena_tls_owns(p)) break;
        ptrs[n++] = p;
    }
    TEST_ASSERT_TRUE(n > 0);

    /* Free every arena-owned allocation — arena should fully drain. */
    for (int i = 0; i < n; i++) {
        bb_arena_tls_free(ptrs[i]);
    }

    /* A fresh allocation must be served FROM THE ARENA again, proving the
     * bump pointer rewound rather than staying permanently consumed. */
    void *p2 = bb_arena_tls_calloc(1, 16);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_TRUE(bb_arena_tls_owns(p2));
    bb_arena_tls_free(p2);
}

/* Double-free is a caller-misuse case (not expected from a well-behaved
 * mbedTLS), but the outstanding-count decrement is guarded against size_t
 * underflow (`if (s_arena_outstanding > 0)`), so a double-free must not
 * crash and must not corrupt arena/offset state for subsequent allocations.
 * Concurrency itself cannot be exercised from a single-threaded host test —
 * the mutex added around both hooks is the guard for that class of bug. */
void test_arena_tls_double_free_does_not_underflow(void)
{
    bb_arena_tls_test_reset();
    bb_arena_tls_init();

    void *p = bb_arena_tls_calloc(1, 16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(bb_arena_tls_owns(p));

    bb_arena_tls_free(p);
    bb_arena_tls_free(p); /* double-free: must not crash or underflow */

    /* Arena state must remain sane: a subsequent alloc still behaves
     * correctly and is served from the arena with an uncorrupted offset. */
    void *p2 = bb_arena_tls_calloc(1, 16);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_TRUE(bb_arena_tls_owns(p2));
    bb_arena_tls_free(p2);
}
