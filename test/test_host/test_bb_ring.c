#include "unity.h"
#include "bb_ring.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define BUF_CAP 256

static bb_ring_t make_ring(size_t cap, size_t max_entry)
{
    bb_ring_t r = NULL;
    bb_err_t err = bb_ring_create(cap, max_entry, &r);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

// ---------------------------------------------------------------------------
// Creation / argument validation
// ---------------------------------------------------------------------------

void test_bb_ring_create_basic(void)
{
    bb_ring_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_create(4, 64, &r));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_bytes_used(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_dropped(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_truncated(r));
    bb_ring_destroy(r);
}

void test_bb_ring_create_zero_capacity_returns_invalid_arg(void)
{
    bb_ring_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_create(0, 64, &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_ring_create_zero_max_entry_returns_invalid_arg(void)
{
    bb_ring_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_create(4, 0, &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_ring_create_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_create(4, 64, NULL));
}

void test_bb_ring_destroy_null_noop(void)
{
    bb_ring_destroy(NULL);  // must not crash
}

// ---------------------------------------------------------------------------
// Alloc failure paths
// ---------------------------------------------------------------------------

void test_bb_ring_create_struct_alloc_fails(void)
{
    bb_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 0;

    bb_ring_t r = NULL;
    bb_err_t err = bb_ring_create(4, 64, &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(r);

    bb_ring_reset_allocator();
}

void test_bb_ring_create_entries_alloc_fails(void)
{
    bb_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 1;

    bb_ring_t r = NULL;
    bb_err_t err = bb_ring_create(4, 64, &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(r);

    bb_ring_reset_allocator();
}

void test_bb_ring_create_payload_alloc_fails(void)
{
    bb_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 2;

    bb_ring_t r = NULL;
    bb_err_t err = bb_ring_create(4, 64, &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(r);

    bb_ring_reset_allocator();
}

// ---------------------------------------------------------------------------
// Push — basic
// ---------------------------------------------------------------------------

void test_bb_ring_push_null_ring_returns_invalid_arg(void)
{
    uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_push(NULL, data, 3, 0, 0));
}

void test_bb_ring_push_null_data_with_nonzero_len_returns_invalid_arg(void)
{
    bb_ring_t r = make_ring(4, 64);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_push(r, NULL, 5, 0, 0));
    bb_ring_destroy(r);
}

void test_bb_ring_push_zero_len_null_data_is_ok(void)
{
    bb_ring_t r = make_ring(4, 64);
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, NULL, 0, 100, 42));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_bytes_used(r));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Push — oversized rejection
// ---------------------------------------------------------------------------

void test_bb_ring_push_oversized_rejected_and_counter_incremented(void)
{
    bb_ring_t r = make_ring(4, 16);
    uint8_t big[32];
    memset(big, 0xAB, sizeof(big));

    bb_err_t err = bb_ring_push(r, big, sizeof(big), 0, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_truncated(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_dropped(r));
    bb_ring_destroy(r);
}

void test_bb_ring_push_exactly_max_entry_bytes_accepted(void)
{
    bb_ring_t r = make_ring(4, 16);
    uint8_t data[16];
    memset(data, 0x55, sizeof(data));

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, data, 16, 0, 1));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_truncated(r));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// FIFO order — push/peek/pop
// ---------------------------------------------------------------------------

void test_bb_ring_fifo_order(void)
{
    bb_ring_t r = make_ring(8, 32);

    for (uint32_t i = 0; i < 5; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "e%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, buf, strlen(buf) + 1, (int64_t)i * 1000, i));
    }

    TEST_ASSERT_EQUAL_size_t(5, bb_ring_count(r));

    for (uint32_t i = 0; i < 5; i++) {
        char buf[32] = {0};
        size_t out_len = 0;
        int64_t out_ts = 0;
        uint32_t out_id = 0xDEAD;

        TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(i, out_id);
        TEST_ASSERT_EQUAL_INT64((int64_t)i * 1000, out_ts);

        char expected[8];
        snprintf(expected, sizeof(expected), "e%u", (unsigned)i);
        TEST_ASSERT_EQUAL_size_t(strlen(expected) + 1, out_len);
        TEST_ASSERT_EQUAL_STRING(expected, buf);

        TEST_ASSERT_EQUAL(BB_OK, bb_ring_pop_oldest(r));
    }

    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Variable-length entries round-trip
// ---------------------------------------------------------------------------

void test_bb_ring_variable_length_entries(void)
{
    bb_ring_t r = make_ring(8, 128);

    // Push entries of varying sizes
    const size_t lens[] = {1, 5, 16, 100, 0, 64, 128};
    const int    num    = (int)(sizeof(lens) / sizeof(lens[0]));
    uint8_t write_buf[128];

    for (int i = 0; i < num; i++) {
        memset(write_buf, (uint8_t)(i + 1), lens[i]);
        const void *d = (lens[i] > 0) ? write_buf : NULL;
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, d, lens[i], (int64_t)i, (uint32_t)i));
    }

    TEST_ASSERT_EQUAL_size_t((size_t)num, bb_ring_count(r));

    for (int i = 0; i < num; i++) {
        uint8_t read_buf[128] = {0};
        size_t out_len = 0;
        int64_t out_ts = 0;
        uint32_t out_id = 0xFF;

        TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, read_buf, sizeof(read_buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_size_t(lens[i], out_len);
        TEST_ASSERT_EQUAL_INT64((int64_t)i, out_ts);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, out_id);

        if (lens[i] > 0) {
            uint8_t expected[128];
            memset(expected, (uint8_t)(i + 1), lens[i]);
            TEST_ASSERT_EQUAL_MEMORY(expected, read_buf, lens[i]);
        }

        TEST_ASSERT_EQUAL(BB_OK, bb_ring_pop_oldest(r));
    }

    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Evict-oldest on overflow + dropped counter
// ---------------------------------------------------------------------------

void test_bb_ring_evict_oldest_on_overflow(void)
{
    bb_ring_t r = make_ring(3, 32);

    for (uint32_t i = 0; i < 5; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "e%u", (unsigned)i);
        bb_ring_push(r, buf, strlen(buf) + 1, (int64_t)i, i);
    }

    // Only last 3 (ids 2,3,4) should remain; 2 dropped
    TEST_ASSERT_EQUAL_size_t(3, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(2, bb_ring_dropped(r));

    for (uint32_t i = 2; i <= 4; i++) {
        char rbuf[32] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, rbuf, sizeof(rbuf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(i, out_id);
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_pop_oldest(r));
    }

    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    bb_ring_destroy(r);
}

void test_bb_ring_dropped_counter_accumulates(void)
{
    bb_ring_t r = make_ring(2, 16);

    // Fill then overflow repeatedly
    for (int i = 0; i < 10; i++) {
        bb_ring_push(r, "x", 1, 0, (uint32_t)i);
    }

    // capacity=2, pushed 10 → count=2, dropped=8
    TEST_ASSERT_EQUAL_size_t(2, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(8, bb_ring_dropped(r));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Timestamp + id round-trip per entry
// ---------------------------------------------------------------------------

void test_bb_ring_ts_and_id_roundtrip(void)
{
    bb_ring_t r = make_ring(4, 8);

    int64_t  ts_vals[] = {0LL, -1LL, INT64_MAX, 123456789LL};
    uint32_t id_vals[] = {0, UINT32_MAX, 0xDEADBEEF, 1};

    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, "hi", 2, ts_vals[i], id_vals[i]));
    }

    for (int i = 0; i < 4; i++) {
        uint8_t buf[8] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_INT64(ts_vals[i], out_ts);
        TEST_ASSERT_EQUAL_UINT32(id_vals[i], out_id);
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_pop_oldest(r));
    }

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// bytes_used tracks correctly
// ---------------------------------------------------------------------------

void test_bb_ring_bytes_used_tracks_push_and_pop(void)
{
    bb_ring_t r = make_ring(8, 64);

    TEST_ASSERT_EQUAL_size_t(0, bb_ring_bytes_used(r));

    bb_ring_push(r, "abc", 3, 0, 1);
    TEST_ASSERT_EQUAL_size_t(3, bb_ring_bytes_used(r));

    bb_ring_push(r, "hello", 5, 0, 2);
    TEST_ASSERT_EQUAL_size_t(8, bb_ring_bytes_used(r));

    bb_ring_pop_oldest(r);
    TEST_ASSERT_EQUAL_size_t(5, bb_ring_bytes_used(r));

    bb_ring_pop_oldest(r);
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_bytes_used(r));

    bb_ring_destroy(r);
}

void test_bb_ring_bytes_used_decrements_on_eviction(void)
{
    bb_ring_t r = make_ring(2, 16);

    bb_ring_push(r, "ab", 2, 0, 1);
    bb_ring_push(r, "cdef", 4, 0, 2);
    TEST_ASSERT_EQUAL_size_t(6, bb_ring_bytes_used(r));

    // Push a third entry — evicts entry 1 (len=2), adds entry 3 (len=1)
    bb_ring_push(r, "x", 1, 0, 3);
    TEST_ASSERT_EQUAL_size_t(5, bb_ring_bytes_used(r));

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Empty-ring peek/pop behaviour
// ---------------------------------------------------------------------------

void test_bb_ring_peek_empty_returns_not_found(void)
{
    bb_ring_t r = make_ring(4, 32);
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    bb_ring_destroy(r);
}

void test_bb_ring_pop_empty_returns_not_found(void)
{
    bb_ring_t r = make_ring(4, 32);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_ring_pop_oldest(r));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_null_ring_returns_invalid_arg(void)
{
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_oldest(NULL, buf, sizeof(buf), &out_len, &out_ts, &out_id));
}

void test_bb_ring_pop_null_ring_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_pop_oldest(NULL));
}

void test_bb_ring_peek_null_out_len_returns_invalid_arg(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "x", 1, 0, 0);
    uint8_t buf[32];
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_oldest(r, buf, sizeof(buf), NULL, &out_ts, &out_id));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_null_out_ts_returns_invalid_arg(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "x", 1, 0, 0);
    uint8_t buf[32];
    size_t out_len;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, NULL, &out_id));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_null_out_id_returns_invalid_arg(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "x", 1, 0, 0);
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, NULL));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Peek with NULL buf / zero buf_cap (probe without copy)
// ---------------------------------------------------------------------------

void test_bb_ring_peek_null_buf_probes_metadata(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "hello", 5, 999, 77);

    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ring_peek_oldest(r, NULL, 0, &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_INT64(999, out_ts);
    TEST_ASSERT_EQUAL_UINT32(77, out_id);

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// clear() resets count and stats
// ---------------------------------------------------------------------------

void test_bb_ring_clear_resets_all(void)
{
    bb_ring_t r = make_ring(3, 16);

    // Fill past capacity to generate drops
    for (int i = 0; i < 6; i++) {
        bb_ring_push(r, "x", 1, 0, (uint32_t)i);
    }
    // Also trigger a truncation
    uint8_t big[32];
    bb_ring_push(r, big, sizeof(big), 0, 99);

    TEST_ASSERT_EQUAL_size_t(3, bb_ring_count(r));
    TEST_ASSERT_NOT_EQUAL(0, bb_ring_dropped(r));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_truncated(r));

    bb_ring_clear(r);

    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_bytes_used(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_dropped(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_truncated(r));

    // Ring should be usable after clear
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, "fresh", 5, 1, 1));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_count(r));

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Head/tail wrap-around (exercising modulo logic)
// ---------------------------------------------------------------------------

void test_bb_ring_head_tail_wrap(void)
{
    bb_ring_t r = make_ring(3, 8);

    // Push 3 to fill, pop 1, push 2 more (head wraps around)
    bb_ring_push(r, "a", 1, 0, 10);
    bb_ring_push(r, "b", 1, 0, 11);
    bb_ring_push(r, "c", 1, 0, 12);
    bb_ring_pop_oldest(r);  // removes id=10
    bb_ring_push(r, "d", 1, 0, 13);
    bb_ring_push(r, "e", 1, 0, 14);  // evicts id=11 (full again)

    TEST_ASSERT_EQUAL_size_t(3, bb_ring_count(r));

    // Expected FIFO order: 12, 13, 14
    uint32_t expected[] = {12, 13, 14};
    for (int i = 0; i < 3; i++) {
        uint8_t buf[8];
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(expected[i], out_id);
        bb_ring_pop_oldest(r);
    }

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// introspection with NULL handle
// ---------------------------------------------------------------------------

void test_bb_ring_introspection_null_returns_zero(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_bytes_used(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_dropped(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_truncated(NULL));
}

// ---------------------------------------------------------------------------
// Peek on a zero-len entry (empty payload)
// ---------------------------------------------------------------------------

void test_bb_ring_peek_zero_len_entry(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, NULL, 0, 42, 99);

    uint8_t buf[32] = {0xAA};
    size_t out_len = 0xFF;
    int64_t out_ts = -1;
    uint32_t out_id = 0xDEAD;

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    TEST_ASSERT_EQUAL_INT64(42, out_ts);
    TEST_ASSERT_EQUAL_UINT32(99, out_id);

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Replay-then-remove pattern (peek → deliver → pop on success)
// ---------------------------------------------------------------------------

void test_bb_ring_peek_deliver_pop_pattern(void)
{
    bb_ring_t r = make_ring(4, 32);

    bb_ring_push(r, "msg1", 4, 1000, 1);
    bb_ring_push(r, "msg2", 4, 2000, 2);
    bb_ring_push(r, "msg3", 4, 3000, 3);

    // Simulate a consumer that peeks, delivers, then pops on success
    // If delivery were to fail we'd leave it in place (peek without pop)
    size_t delivered = 0;
    while (bb_ring_count(r) > 0) {
        char buf[32] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        bb_err_t err = bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id);
        TEST_ASSERT_EQUAL(BB_OK, err);

        // Simulate successful delivery: pop
        TEST_ASSERT_EQUAL(BB_OK, bb_ring_pop_oldest(r));
        delivered++;
    }

    TEST_ASSERT_EQUAL_size_t(3, delivered);
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_count(r));
    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// buf_cap smaller than entry: partial copy
// ---------------------------------------------------------------------------

void test_bb_ring_peek_truncated_copy_on_small_buf(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "hello world", 11, 0, 1);

    uint8_t buf[4] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    // out_len reports full entry length
    TEST_ASSERT_EQUAL_size_t(11, out_len);
    // buf gets only the first 4 bytes
    TEST_ASSERT_EQUAL_MEMORY("hell", buf, 4);

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Allocator set with NULL args falls back to calloc/free (branch coverage)
// ---------------------------------------------------------------------------

void test_bb_ring_set_allocator_null_args_falls_back_to_default(void)
{
    // Pass NULL for both — should fall back to stdlib calloc/free
    bb_ring_set_allocator(NULL, NULL);

    bb_ring_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_create(2, 8, &r));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, "hi", 2, 0, 1));
    bb_ring_destroy(r);
    // restore (already default)
    bb_ring_reset_allocator();
}

// ---------------------------------------------------------------------------
// bb_ring_clear(NULL) is a no-op (NULL guard branch)
// ---------------------------------------------------------------------------

void test_bb_ring_clear_null_noop(void)
{
    bb_ring_clear(NULL);  // must not crash
}

// ---------------------------------------------------------------------------
// Peek with non-NULL buf but buf_cap=0 — no copy, zero-len entry path
// ---------------------------------------------------------------------------

void test_bb_ring_peek_nonzero_len_with_zero_buf_cap_skips_copy(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "data", 4, 0, 5);

    // buf is non-NULL but buf_cap=0 → no memcpy (branch: buf_cap > 0 is false)
    uint8_t sentinel[8];
    memset(sentinel, 0xCC, sizeof(sentinel));
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ring_peek_oldest(r, sentinel, 0, &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    // sentinel should be untouched since buf_cap=0
    uint8_t expected[8];
    memset(expected, 0xCC, sizeof(expected));
    TEST_ASSERT_EQUAL_MEMORY(expected, sentinel, sizeof(sentinel));

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// Peek with non-NULL buf, buf_cap > 0, but zero-len entry — no copy
// ---------------------------------------------------------------------------

void test_bb_ring_peek_with_buf_on_zero_len_entry_skips_copy(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, NULL, 0, 0, 7);

    uint8_t sentinel[8];
    memset(sentinel, 0xBB, sizeof(sentinel));
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ring_peek_oldest(r, sentinel, sizeof(sentinel), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    // sentinel should be untouched since e->len=0
    uint8_t expected[8];
    memset(expected, 0xBB, sizeof(expected));
    TEST_ASSERT_EQUAL_MEMORY(expected, sentinel, sizeof(sentinel));

    bb_ring_destroy(r);
}

// ---------------------------------------------------------------------------
// bb_ring_peek_at — non-destructive indexed read
// ---------------------------------------------------------------------------

void test_bb_ring_peek_at_null_ring_returns_invalid_arg(void)
{
    uint8_t buf[8];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_at(NULL, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
}

void test_bb_ring_peek_at_null_out_ptr_returns_invalid_arg(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "x", 1, 0, 1);
    uint8_t buf[8];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_at(r, 0, buf, sizeof(buf), NULL, &out_ts, &out_id));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_at(r, 0, buf, sizeof(buf), &out_len, NULL, &out_id));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ring_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, NULL));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_empty_ring_returns_not_found(void)
{
    bb_ring_t r = make_ring(4, 32);
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_ring_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_index_out_of_range_returns_not_found(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "a", 1, 10, 1);
    bb_ring_push(r, "b", 1, 20, 2);

    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    // count=2, so index 2 is out of range
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_ring_peek_at(r, 2, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_index_0_is_oldest(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "first", 5, 100, 10);
    bb_ring_push(r, "second", 6, 200, 20);
    bb_ring_push(r, "third", 5, 300, 30);

    uint8_t buf[32] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ring_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(10, out_id);
    TEST_ASSERT_EQUAL_INT64(100, out_ts);
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_STRING("first", (const char *)buf);

    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_last_index_is_newest(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "first", 5, 100, 10);
    bb_ring_push(r, "second", 6, 200, 20);
    bb_ring_push(r, "third", 5, 300, 30);

    uint8_t buf[32] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    // index 2 = newest (count-1)
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ring_peek_at(r, 2, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(30, out_id);
    TEST_ASSERT_EQUAL_INT64(300, out_ts);
    TEST_ASSERT_EQUAL_STRING("third", (const char *)buf);

    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_all_entries_in_order(void)
{
    bb_ring_t r = make_ring(8, 32);
    const char *strs[] = {"zero", "one", "two", "three", "four"};
    int n = (int)(sizeof(strs) / sizeof(strs[0]));

    for (int i = 0; i < n; i++) {
        bb_ring_push(r, strs[i], strlen(strs[i]) + 1, (int64_t)(i * 10), (uint32_t)i);
    }

    for (int i = 0; i < n; i++) {
        uint8_t buf[32] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        TEST_ASSERT_EQUAL(BB_OK,
                          bb_ring_peek_at(r, (size_t)i, buf, sizeof(buf),
                                          &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, out_id);
        TEST_ASSERT_EQUAL_INT64((int64_t)(i * 10), out_ts);
        TEST_ASSERT_EQUAL_STRING(strs[i], (const char *)buf);
    }

    // Ring count must be unchanged (non-destructive).
    TEST_ASSERT_EQUAL_size_t((size_t)n, bb_ring_count(r));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_is_nondestructive(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "msg", 3, 1, 42);

    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    // Peek at same entry multiple times — count must not change.
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(BB_OK,
                          bb_ring_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(42, out_id);
    }
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_count(r));

    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_after_wrap_around(void)
{
    // Push more entries than capacity to force head/tail wrap.
    bb_ring_t r = make_ring(3, 8);

    // Fill to capacity and overflow by 2 (evicts 0 and 1).
    for (uint32_t i = 0; i < 5; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "e%u", (unsigned)i);
        bb_ring_push(r, buf, strlen(buf) + 1, (int64_t)i, i);
    }

    // Ring holds entries 2, 3, 4 (oldest→newest).
    TEST_ASSERT_EQUAL_size_t(3, bb_ring_count(r));

    uint32_t expected_ids[] = {2, 3, 4};
    for (int i = 0; i < 3; i++) {
        char buf[8] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        TEST_ASSERT_EQUAL(BB_OK,
                          bb_ring_peek_at(r, (size_t)i, buf, sizeof(buf),
                                          &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(expected_ids[i], out_id);
    }

    // Still non-destructive after all reads.
    TEST_ASSERT_EQUAL_size_t(3, bb_ring_count(r));
    bb_ring_destroy(r);
}

void test_bb_ring_peek_at_null_buf_probes_metadata(void)
{
    bb_ring_t r = make_ring(4, 32);
    bb_ring_push(r, "hello", 5, 999, 77);

    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ring_peek_at(r, 0, NULL, 0, &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_INT64(999, out_ts);
    TEST_ASSERT_EQUAL_UINT32(77, out_id);

    bb_ring_destroy(r);
}
