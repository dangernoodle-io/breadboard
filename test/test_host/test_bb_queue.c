#include "unity.h"
#include "bb_queue.h"
#include "bb_queue_test.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define BUF_CAP 256

static bb_queue_t make_ring(size_t cap, size_t max_entry)
{
    bb_queue_t r = NULL;
    bb_err_t err = bb_queue_create(cap, max_entry, BB_QUEUE_EVICT_OLDEST, "test", &r);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

static bb_queue_t make_ring_ex(size_t cap, size_t max_entry,
                              bb_queue_full_policy_t policy,
                              size_t max_bytes, uint32_t max_age)
{
    bb_queue_cfg_t cfg = {
        .capacity_entries = cap,
        .max_entry_bytes  = max_entry,
        .policy           = policy,
        .name             = "test_ex",
        .max_bytes        = max_bytes,
        .max_age       = max_age,
    };
    bb_queue_t r = NULL;
    bb_err_t err = bb_queue_create_ex(&cfg, &r);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

// ---------------------------------------------------------------------------
// Creation / argument validation
// ---------------------------------------------------------------------------

void test_bb_queue_create_basic(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_create(4, 64, BB_QUEUE_EVICT_OLDEST, "basic", &r));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(4, bb_queue_capacity(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_dropped(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_truncated(r));
    TEST_ASSERT_EQUAL_STRING("basic", bb_queue_name(r));
    bb_queue_destroy(r);
}

void test_bb_queue_create_zero_capacity_returns_invalid_arg(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_create(0, 64, BB_QUEUE_EVICT_OLDEST, "t", &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_queue_create_zero_max_entry_returns_invalid_arg(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_create(4, 0, BB_QUEUE_EVICT_OLDEST, "t", &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_queue_create_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_create(4, 64, BB_QUEUE_EVICT_OLDEST, "t", NULL));
}

void test_bb_queue_destroy_null_noop(void)
{
    bb_queue_destroy(NULL);  // must not crash
}

// ---------------------------------------------------------------------------
// Alloc failure paths
// ---------------------------------------------------------------------------

void test_bb_queue_create_struct_alloc_fails(void)
{
    bb_queue_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 0;

    bb_queue_t r = NULL;
    bb_err_t err = bb_queue_create(4, 64, BB_QUEUE_EVICT_OLDEST, "t", &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(r);

    bb_queue_reset_allocator();
}

void test_bb_queue_create_entries_alloc_fails(void)
{
    bb_queue_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 1;

    bb_queue_t r = NULL;
    bb_err_t err = bb_queue_create(4, 64, BB_QUEUE_EVICT_OLDEST, "t", &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(r);

    bb_queue_reset_allocator();
}

void test_bb_queue_create_payload_alloc_fails(void)
{
    bb_queue_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 2;

    bb_queue_t r = NULL;
    bb_err_t err = bb_queue_create(4, 64, BB_QUEUE_EVICT_OLDEST, "t", &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(r);

    bb_queue_reset_allocator();
}

// ---------------------------------------------------------------------------
// Push — basic
// ---------------------------------------------------------------------------

void test_bb_queue_push_null_ring_returns_invalid_arg(void)
{
    uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_push(NULL, data, 3, 0, 0));
}

void test_bb_queue_push_null_data_with_nonzero_len_returns_invalid_arg(void)
{
    bb_queue_t r = make_ring(4, 64);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_push(r, NULL, 5, 0, 0));
    bb_queue_destroy(r);
}

void test_bb_queue_push_zero_len_null_data_is_ok(void)
{
    bb_queue_t r = make_ring(4, 64);
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, NULL, 0, 100, 42));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Push — oversized rejection
// ---------------------------------------------------------------------------

void test_bb_queue_push_oversized_rejected_and_counter_incremented(void)
{
    bb_queue_t r = make_ring(4, 16);
    uint8_t big[32];
    memset(big, 0xAB, sizeof(big));

    bb_err_t err = bb_queue_push(r, big, sizeof(big), 0, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_truncated(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_dropped(r));
    bb_queue_destroy(r);
}

void test_bb_queue_push_exactly_max_entry_bytes_accepted(void)
{
    bb_queue_t r = make_ring(4, 16);
    uint8_t data[16];
    memset(data, 0x55, sizeof(data));

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, data, 16, 0, 1));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_truncated(r));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// FIFO order — push/peek/pop
// ---------------------------------------------------------------------------

void test_bb_queue_fifo_order(void)
{
    bb_queue_t r = make_ring(8, 32);

    for (uint32_t i = 0; i < 5; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "e%u", (unsigned)i);
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, buf, strlen(buf) + 1, (int64_t)i * 1000, i));
    }

    TEST_ASSERT_EQUAL_size_t(5, bb_queue_count(r));

    for (uint32_t i = 0; i < 5; i++) {
        char buf[32] = {0};
        size_t out_len = 0;
        int64_t out_ts = 0;
        uint32_t out_id = 0xDEAD;

        TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(i, out_id);
        TEST_ASSERT_EQUAL_INT64((int64_t)i * 1000, out_ts);

        char expected[8];
        snprintf(expected, sizeof(expected), "e%u", (unsigned)i);
        TEST_ASSERT_EQUAL_size_t(strlen(expected) + 1, out_len);
        TEST_ASSERT_EQUAL_STRING(expected, buf);

        TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));
    }

    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Variable-length entries round-trip
// ---------------------------------------------------------------------------

void test_bb_queue_variable_length_entries(void)
{
    bb_queue_t r = make_ring(8, 128);

    // Push entries of varying sizes
    const size_t lens[] = {1, 5, 16, 100, 0, 64, 128};
    const int    num    = (int)(sizeof(lens) / sizeof(lens[0]));
    uint8_t write_buf[128];

    for (int i = 0; i < num; i++) {
        memset(write_buf, (uint8_t)(i + 1), lens[i]);
        const void *d = (lens[i] > 0) ? write_buf : NULL;
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, d, lens[i], (int64_t)i, (uint32_t)i));
    }

    TEST_ASSERT_EQUAL_size_t((size_t)num, bb_queue_count(r));

    for (int i = 0; i < num; i++) {
        uint8_t read_buf[128] = {0};
        size_t out_len = 0;
        int64_t out_ts = 0;
        uint32_t out_id = 0xFF;

        TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, read_buf, sizeof(read_buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_size_t(lens[i], out_len);
        TEST_ASSERT_EQUAL_INT64((int64_t)i, out_ts);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, out_id);

        if (lens[i] > 0) {
            uint8_t expected[128];
            memset(expected, (uint8_t)(i + 1), lens[i]);
            TEST_ASSERT_EQUAL_MEMORY(expected, read_buf, lens[i]);
        }

        TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));
    }

    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Evict-oldest on overflow + dropped counter
// ---------------------------------------------------------------------------

void test_bb_queue_evict_oldest_on_overflow(void)
{
    bb_queue_t r = make_ring(3, 32);

    for (uint32_t i = 0; i < 5; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "e%u", (unsigned)i);
        bb_queue_push(r, buf, strlen(buf) + 1, (int64_t)i, i);
    }

    // Only last 3 (ids 2,3,4) should remain; 2 dropped
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_dropped(r));

    for (uint32_t i = 2; i <= 4; i++) {
        char rbuf[32] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, rbuf, sizeof(rbuf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(i, out_id);
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));
    }

    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    bb_queue_destroy(r);
}

void test_bb_queue_dropped_counter_accumulates(void)
{
    bb_queue_t r = make_ring(2, 16);

    // Fill then overflow repeatedly
    for (int i = 0; i < 10; i++) {
        bb_queue_push(r, "x", 1, 0, (uint32_t)i);
    }

    // capacity=2, pushed 10 → count=2, dropped=8
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(8, bb_queue_dropped(r));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Timestamp + id round-trip per entry
// ---------------------------------------------------------------------------

void test_bb_queue_ts_and_id_roundtrip(void)
{
    bb_queue_t r = make_ring(4, 8);

    int64_t  ts_vals[] = {0LL, -1LL, INT64_MAX, 123456789LL};
    uint32_t id_vals[] = {0, UINT32_MAX, 0xDEADBEEF, 1};

    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "hi", 2, ts_vals[i], id_vals[i]));
    }

    for (int i = 0; i < 4; i++) {
        uint8_t buf[8] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_INT64(ts_vals[i], out_ts);
        TEST_ASSERT_EQUAL_UINT32(id_vals[i], out_id);
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));
    }

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// bytes_used tracks correctly
// ---------------------------------------------------------------------------

void test_bb_queue_bytes_used_tracks_push_and_pop(void)
{
    bb_queue_t r = make_ring(8, 64);

    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));

    bb_queue_push(r, "abc", 3, 0, 1);
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_bytes_used(r));

    bb_queue_push(r, "hello", 5, 0, 2);
    TEST_ASSERT_EQUAL_size_t(8, bb_queue_bytes_used(r));

    bb_queue_pop_oldest(r);
    TEST_ASSERT_EQUAL_size_t(5, bb_queue_bytes_used(r));

    bb_queue_pop_oldest(r);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));

    bb_queue_destroy(r);
}

void test_bb_queue_bytes_used_decrements_on_eviction(void)
{
    bb_queue_t r = make_ring(2, 16);

    bb_queue_push(r, "ab", 2, 0, 1);
    bb_queue_push(r, "cdef", 4, 0, 2);
    TEST_ASSERT_EQUAL_size_t(6, bb_queue_bytes_used(r));

    // Push a third entry — evicts entry 1 (len=2), adds entry 3 (len=1)
    bb_queue_push(r, "x", 1, 0, 3);
    TEST_ASSERT_EQUAL_size_t(5, bb_queue_bytes_used(r));

    bb_queue_destroy(r);
}

// B1-473: bytes_used must never wrap on pop/evict, even under accounting
// drift. push()/pop_oldest() keep bytes_used symmetric by construction (every
// push adds len exactly once; every pop/evict subtracts the removed entry's
// len exactly once), so these tests inject drift via the BB_QUEUE_TESTING
// hook to force the underflow-guard branch (bb_queue_bytes_used_sub's
// clamp-to-0 path) to actually execute.

void test_bb_queue_bytes_used_empty_ring_is_zero(void)
{
    bb_queue_t r = make_ring(4, 32);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));
    bb_queue_destroy(r);
}

void test_bb_queue_bytes_used_clamps_on_pop_underflow(void)
{
    bb_queue_t r = make_ring(4, 32);

    bb_queue_push(r, "hi", 2, 0, 1);
    // Force drift: bytes_used (1) is now less than the entry's real len (2).
    bb_queue_test_force_bytes_used(r, 1);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));
    // Clamped to 0, never wraps to SIZE_MAX.
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));

    bb_queue_destroy(r);
}

void test_bb_queue_bytes_used_clamps_on_evict_underflow(void)
{
    bb_queue_t r = make_ring(2, 16);

    bb_queue_push(r, "ab", 2, 0, 1);
    bb_queue_push(r, "cdef", 4, 0, 2);
    // Force drift: bytes_used (1) is less than the oldest entry's len (2)
    // that eviction is about to subtract.
    bb_queue_test_force_bytes_used(r, 1);

    // Ring is full (count==capacity==2); this push evicts entry 1 (len=2).
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "x", 1, 0, 3));

    // Clamp fires on the evict subtract (1 - 2 -> 0), then push adds 1.
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_bytes_used(r));

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Empty-ring peek/pop behaviour
// ---------------------------------------------------------------------------

void test_bb_queue_peek_empty_returns_not_found(void)
{
    bb_queue_t r = make_ring(4, 32);
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    bb_queue_destroy(r);
}

void test_bb_queue_pop_empty_returns_not_found(void)
{
    bb_queue_t r = make_ring(4, 32);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_queue_pop_oldest(r));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_null_ring_returns_invalid_arg(void)
{
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_oldest(NULL, buf, sizeof(buf), &out_len, &out_ts, &out_id));
}

void test_bb_queue_pop_null_ring_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_pop_oldest(NULL));
}

void test_bb_queue_peek_null_out_len_returns_invalid_arg(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "x", 1, 0, 0);
    uint8_t buf[32];
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_oldest(r, buf, sizeof(buf), NULL, &out_ts, &out_id));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_null_out_ts_returns_invalid_arg(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "x", 1, 0, 0);
    uint8_t buf[32];
    size_t out_len;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, NULL, &out_id));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_null_out_id_returns_invalid_arg(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "x", 1, 0, 0);
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, NULL));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Peek with NULL buf / zero buf_cap (probe without copy)
// ---------------------------------------------------------------------------

void test_bb_queue_peek_null_buf_probes_metadata(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "hello", 5, 999, 77);

    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_oldest(r, NULL, 0, &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_INT64(999, out_ts);
    TEST_ASSERT_EQUAL_UINT32(77, out_id);

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// clear() resets structural state; cumulative counters survive
// ---------------------------------------------------------------------------

void test_bb_queue_clear_resets_all(void)
{
    bb_queue_t r = make_ring(3, 16);

    // Fill past capacity to generate drops
    for (int i = 0; i < 6; i++) {
        bb_queue_push(r, "x", 1, 0, (uint32_t)i);
    }
    // Also trigger a truncation
    uint8_t big[32];
    bb_queue_push(r, big, sizeof(big), 0, 99);

    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));
    size_t dropped_before   = bb_queue_dropped(r);
    size_t truncated_before = bb_queue_truncated(r);
    TEST_ASSERT_NOT_EQUAL(0, dropped_before);
    TEST_ASSERT_EQUAL_size_t(1, truncated_before);

    bb_queue_clear(r);

    // Structural state reset
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));

    // Cumulative diagnostics survive clear
    TEST_ASSERT_EQUAL_size_t(dropped_before,   bb_queue_dropped(r));
    TEST_ASSERT_EQUAL_size_t(truncated_before, bb_queue_truncated(r));

    // Ring should be usable after clear
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "fresh", 5, 1, 1));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));

    bb_queue_destroy(r);
}

void test_bb_queue_clear_preserves_counters_after_evict_dropped(void)
{
    // Use REJECT_NEW so we can distinguish clear from create zeroing.
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(2, 16, BB_QUEUE_REJECT_NEW, "clear_test", &r));

    bb_queue_push(r, "a", 1, 0, 1);
    bb_queue_push(r, "b", 1, 0, 2);
    // Ring full — two rejects → dropped = 2
    bb_queue_push(r, "c", 1, 0, 3);
    bb_queue_push(r, "d", 1, 0, 4);
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_dropped(r));

    bb_queue_clear(r);

    // Structural state reset
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(r));

    // dropped counter must survive
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_dropped(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_truncated(r));

    // Ring fully usable after clear
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "x", 1, 0, 10));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// NULL-ring push under REJECT_NEW policy
// ---------------------------------------------------------------------------

void test_bb_queue_reject_new_null_ring_returns_invalid_arg(void)
{
    uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_push(NULL, data, sizeof(data), 0, 0));
}

// ---------------------------------------------------------------------------
// Head/tail wrap-around (exercising modulo logic)
// ---------------------------------------------------------------------------

void test_bb_queue_head_tail_wrap(void)
{
    bb_queue_t r = make_ring(3, 8);

    // Push 3 to fill, pop 1, push 2 more (head wraps around)
    bb_queue_push(r, "a", 1, 0, 10);
    bb_queue_push(r, "b", 1, 0, 11);
    bb_queue_push(r, "c", 1, 0, 12);
    bb_queue_pop_oldest(r);  // removes id=10
    bb_queue_push(r, "d", 1, 0, 13);
    bb_queue_push(r, "e", 1, 0, 14);  // evicts id=11 (full again)

    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));

    // Expected FIFO order: 12, 13, 14
    uint32_t expected[] = {12, 13, 14};
    for (int i = 0; i < 3; i++) {
        uint8_t buf[8];
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(expected[i], out_id);
        bb_queue_pop_oldest(r);
    }

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// introspection with NULL handle
// ---------------------------------------------------------------------------

void test_bb_queue_introspection_null_returns_zero(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_capacity(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_bytes_used(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_dropped(NULL));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_truncated(NULL));
}

// ---------------------------------------------------------------------------
// Peek on a zero-len entry (empty payload)
// ---------------------------------------------------------------------------

void test_bb_queue_peek_zero_len_entry(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, NULL, 0, 42, 99);

    uint8_t buf[32] = {0xAA};
    size_t out_len = 0xFF;
    int64_t out_ts = -1;
    uint32_t out_id = 0xDEAD;

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    TEST_ASSERT_EQUAL_INT64(42, out_ts);
    TEST_ASSERT_EQUAL_UINT32(99, out_id);

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Replay-then-remove pattern (peek → deliver → pop on success)
// ---------------------------------------------------------------------------

void test_bb_queue_peek_deliver_pop_pattern(void)
{
    bb_queue_t r = make_ring(4, 32);

    bb_queue_push(r, "msg1", 4, 1000, 1);
    bb_queue_push(r, "msg2", 4, 2000, 2);
    bb_queue_push(r, "msg3", 4, 3000, 3);

    // Simulate a consumer that peeks, delivers, then pops on success
    // If delivery were to fail we'd leave it in place (peek without pop)
    size_t delivered = 0;
    while (bb_queue_count(r) > 0) {
        char buf[32] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        bb_err_t err = bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id);
        TEST_ASSERT_EQUAL(BB_OK, err);

        // Simulate successful delivery: pop
        TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));
        delivered++;
    }

    TEST_ASSERT_EQUAL_size_t(3, delivered);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// buf_cap smaller than entry: partial copy
// ---------------------------------------------------------------------------

void test_bb_queue_peek_truncated_copy_on_small_buf(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "hello world", 11, 0, 1);

    uint8_t buf[4] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    // out_len reports full entry length
    TEST_ASSERT_EQUAL_size_t(11, out_len);
    // buf gets only the first 4 bytes
    TEST_ASSERT_EQUAL_MEMORY("hell", buf, 4);

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Allocator set with NULL args falls back to calloc/free (branch coverage)
// ---------------------------------------------------------------------------

void test_bb_queue_set_allocator_null_args_falls_back_to_default(void)
{
    // Pass NULL for both — should fall back to stdlib calloc/free
    bb_queue_set_allocator(NULL, NULL);

    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_create(2, 8, BB_QUEUE_EVICT_OLDEST, "alloc_t", &r));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "hi", 2, 0, 1));
    bb_queue_destroy(r);
    // restore (already default)
    bb_queue_reset_allocator();
}

// ---------------------------------------------------------------------------
// bb_queue_clear(NULL) is a no-op (NULL guard branch)
// ---------------------------------------------------------------------------

void test_bb_queue_clear_null_noop(void)
{
    bb_queue_clear(NULL);  // must not crash
}

// ---------------------------------------------------------------------------
// Peek with non-NULL buf but buf_cap=0 — no copy, zero-len entry path
// ---------------------------------------------------------------------------

void test_bb_queue_peek_nonzero_len_with_zero_buf_cap_skips_copy(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "data", 4, 0, 5);

    // buf is non-NULL but buf_cap=0 → no memcpy (branch: buf_cap > 0 is false)
    uint8_t sentinel[8];
    memset(sentinel, 0xCC, sizeof(sentinel));
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_oldest(r, sentinel, 0, &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    // sentinel should be untouched since buf_cap=0
    uint8_t expected[8];
    memset(expected, 0xCC, sizeof(expected));
    TEST_ASSERT_EQUAL_MEMORY(expected, sentinel, sizeof(sentinel));

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Peek with non-NULL buf, buf_cap > 0, but zero-len entry — no copy
// ---------------------------------------------------------------------------

void test_bb_queue_peek_with_buf_on_zero_len_entry_skips_copy(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, NULL, 0, 0, 7);

    uint8_t sentinel[8];
    memset(sentinel, 0xBB, sizeof(sentinel));
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_oldest(r, sentinel, sizeof(sentinel), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    // sentinel should be untouched since e->len=0
    uint8_t expected[8];
    memset(expected, 0xBB, sizeof(expected));
    TEST_ASSERT_EQUAL_MEMORY(expected, sentinel, sizeof(sentinel));

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// bb_queue_peek_at — non-destructive indexed read
// ---------------------------------------------------------------------------

void test_bb_queue_peek_at_null_ring_returns_invalid_arg(void)
{
    uint8_t buf[8];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_at(NULL, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
}

void test_bb_queue_peek_at_null_out_ptr_returns_invalid_arg(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "x", 1, 0, 1);
    uint8_t buf[8];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_at(r, 0, buf, sizeof(buf), NULL, &out_ts, &out_id));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_at(r, 0, buf, sizeof(buf), &out_len, NULL, &out_id));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, NULL));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_empty_ring_returns_not_found(void)
{
    bb_queue_t r = make_ring(4, 32);
    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_queue_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_index_out_of_range_returns_not_found(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "a", 1, 10, 1);
    bb_queue_push(r, "b", 1, 20, 2);

    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    // count=2, so index 2 is out of range
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_queue_peek_at(r, 2, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_index_0_is_oldest(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "first", 5, 100, 10);
    bb_queue_push(r, "second", 6, 200, 20);
    bb_queue_push(r, "third", 5, 300, 30);

    uint8_t buf[32] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(10, out_id);
    TEST_ASSERT_EQUAL_INT64(100, out_ts);
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_STRING("first", (const char *)buf);

    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_last_index_is_newest(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "first", 5, 100, 10);
    bb_queue_push(r, "second", 6, 200, 20);
    bb_queue_push(r, "third", 5, 300, 30);

    uint8_t buf[32] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    // index 2 = newest (count-1)
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_at(r, 2, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(30, out_id);
    TEST_ASSERT_EQUAL_INT64(300, out_ts);
    TEST_ASSERT_EQUAL_STRING("third", (const char *)buf);

    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_all_entries_in_order(void)
{
    bb_queue_t r = make_ring(8, 32);
    const char *strs[] = {"zero", "one", "two", "three", "four"};
    int n = (int)(sizeof(strs) / sizeof(strs[0]));

    for (int i = 0; i < n; i++) {
        bb_queue_push(r, strs[i], strlen(strs[i]) + 1, (int64_t)(i * 10), (uint32_t)i);
    }

    for (int i = 0; i < n; i++) {
        uint8_t buf[32] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        TEST_ASSERT_EQUAL(BB_OK,
                          bb_queue_peek_at(r, (size_t)i, buf, sizeof(buf),
                                          &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, out_id);
        TEST_ASSERT_EQUAL_INT64((int64_t)(i * 10), out_ts);
        TEST_ASSERT_EQUAL_STRING(strs[i], (const char *)buf);
    }

    // Ring count must be unchanged (non-destructive).
    TEST_ASSERT_EQUAL_size_t((size_t)n, bb_queue_count(r));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_is_nondestructive(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "msg", 3, 1, 42);

    uint8_t buf[32];
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    // Peek at same entry multiple times — count must not change.
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(BB_OK,
                          bb_queue_peek_at(r, 0, buf, sizeof(buf), &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(42, out_id);
    }
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));

    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_after_wrap_around(void)
{
    // Push more entries than capacity to force head/tail wrap.
    bb_queue_t r = make_ring(3, 8);

    // Fill to capacity and overflow by 2 (evicts 0 and 1).
    for (uint32_t i = 0; i < 5; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "e%u", (unsigned)i);
        bb_queue_push(r, buf, strlen(buf) + 1, (int64_t)i, i);
    }

    // Ring holds entries 2, 3, 4 (oldest→newest).
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));

    uint32_t expected_ids[] = {2, 3, 4};
    for (int i = 0; i < 3; i++) {
        char buf[8] = {0};
        size_t out_len;
        int64_t out_ts;
        uint32_t out_id;

        TEST_ASSERT_EQUAL(BB_OK,
                          bb_queue_peek_at(r, (size_t)i, buf, sizeof(buf),
                                          &out_len, &out_ts, &out_id));
        TEST_ASSERT_EQUAL_UINT32(expected_ids[i], out_id);
    }

    // Still non-destructive after all reads.
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));
    bb_queue_destroy(r);
}

void test_bb_queue_peek_at_null_buf_probes_metadata(void)
{
    bb_queue_t r = make_ring(4, 32);
    bb_queue_push(r, "hello", 5, 999, 77);

    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_at(r, 0, NULL, 0, &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_INT64(999, out_ts);
    TEST_ASSERT_EQUAL_UINT32(77, out_id);

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// BB_QUEUE_REJECT_NEW policy (single named constructor)
// ---------------------------------------------------------------------------

void test_bb_queue_create_evict_oldest(void)
{
    // Verify EVICT_OLDEST policy works via the unified constructor.
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(4, 32, BB_QUEUE_EVICT_OLDEST, "evict_test", &r));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_count(r));

    // Overflow: should evict (not reject)
    for (uint32_t i = 0; i < 6; i++) {
        bb_queue_push(r, "x", 1, 0, i);
    }
    TEST_ASSERT_EQUAL_size_t(4, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_dropped(r));

    bb_queue_destroy(r);
}

void test_bb_queue_create_invalid_policy_returns_invalid_arg(void)
{
    bb_queue_t r = NULL;
    // policy value 99 is not a recognised enum value
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_queue_create(4, 32, (bb_queue_full_policy_t)99, "t", &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_queue_reject_new_push_on_full_returns_no_space(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(3, 16, BB_QUEUE_REJECT_NEW, "reject_full", &r));
    TEST_ASSERT_NOT_NULL(r);

    // Fill to capacity
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "aaa", 3, 1, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "bbb", 3, 2, 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "ccc", 3, 3, 3));
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_dropped(r));

    // One more push must be rejected
    bb_err_t err = bb_queue_push(r, "ddd", 3, 4, 4);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    // Count must not change
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));

    // Dropped counter must have incremented
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_dropped(r));

    bb_queue_destroy(r);
}

void test_bb_queue_reject_new_oldest_entry_intact_after_reject(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(2, 32, BB_QUEUE_REJECT_NEW, "reject_intact", &r));

    bb_queue_push(r, "first", 5, 100, 10);
    bb_queue_push(r, "second", 6, 200, 20);

    // Ring is full — reject new entry
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_queue_push(r, "third", 5, 300, 30));

    // Oldest entry (id=10, "first") must still be the oldest (not evicted)
    char buf[32] = {0};
    size_t out_len;
    int64_t out_ts;
    uint32_t out_id;

    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(10, out_id);
    TEST_ASSERT_EQUAL_INT64(100, out_ts);
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_STRING("first", buf);

    bb_queue_destroy(r);
}

void test_bb_queue_reject_new_multiple_rejects_accumulate_dropped(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(2, 16, BB_QUEUE_REJECT_NEW, "reject_multi", &r));

    bb_queue_push(r, "a", 1, 0, 1);
    bb_queue_push(r, "b", 1, 0, 2);

    // Five rejections
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_queue_push(r, "x", 1, 0, (uint32_t)(10 + i)));
    }

    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(5, bb_queue_dropped(r));

    bb_queue_destroy(r);
}

void test_bb_queue_reject_new_pop_then_push_succeeds(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(2, 16, BB_QUEUE_REJECT_NEW, "reject_pop", &r));

    bb_queue_push(r, "a", 1, 0, 1);
    bb_queue_push(r, "b", 1, 0, 2);

    // Full — reject
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_queue_push(r, "c", 1, 0, 3));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_dropped(r));

    // Pop one slot free
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_pop_oldest(r));

    // Now push should succeed
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "c", 1, 0, 3));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r));

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// bb_queue_name accessor
// ---------------------------------------------------------------------------

void test_bb_queue_name_returns_stored_name(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(4, 16, BB_QUEUE_EVICT_OLDEST, "myring", &r));
    TEST_ASSERT_EQUAL_STRING("myring", bb_queue_name(r));
    bb_queue_destroy(r);
}

void test_bb_queue_name_null_ring_returns_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", bb_queue_name(NULL));
}

void test_bb_queue_name_null_name_stores_empty(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(4, 16, BB_QUEUE_EVICT_OLDEST, NULL, &r));
    TEST_ASSERT_EQUAL_STRING("", bb_queue_name(r));
    bb_queue_destroy(r);
}

void test_bb_queue_name_truncated_at_limit(void)
{
    // Name longer than BB_QUEUE_NAME_MAX - 1 must be silently truncated.
    const char *long_name = "this_name_is_definitely_longer_than_24_chars";
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_queue_create(2, 8, BB_QUEUE_EVICT_OLDEST, long_name, &r));
    // Stored name must be exactly BB_QUEUE_NAME_MAX - 1 chars, NUL-terminated.
    TEST_ASSERT_EQUAL_size_t(BB_QUEUE_NAME_MAX - 1, strlen(bb_queue_name(r)));
    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// bb_queue_create_ex — argument validation (B1-1031)
// ---------------------------------------------------------------------------

void test_bb_queue_create_ex_null_cfg_returns_invalid_arg(void)
{
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_create_ex(NULL, &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_queue_create_ex_zero_capacity_returns_invalid_arg(void)
{
    bb_queue_cfg_t cfg = {
        .capacity_entries = 0,
        .max_entry_bytes  = 64,
        .policy           = BB_QUEUE_EVICT_OLDEST,
        .name             = "t",
        .max_bytes        = 0,
        .max_age       = 0,
    };
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_create_ex(&cfg, &r));
    TEST_ASSERT_NULL(r);
}

void test_bb_queue_create_ex_invalid_policy_returns_invalid_arg(void)
{
    bb_queue_cfg_t cfg = {
        .capacity_entries = 4,
        .max_entry_bytes  = 32,
        .policy           = (bb_queue_full_policy_t)99,
        .name             = "t",
        .max_bytes        = 0,
        .max_age       = 0,
    };
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_create_ex(&cfg, &r));
    TEST_ASSERT_NULL(r);
}

// ---------------------------------------------------------------------------
// Disabled path == legacy bb_queue_create (canary — zero regression)
// ---------------------------------------------------------------------------

void test_bb_queue_disabled_matches_legacy(void)
{
    bb_queue_t legacy = make_ring(3, 8);
    bb_queue_t ex     = make_ring_ex(3, 8, BB_QUEUE_EVICT_OLDEST, 0, 0);

    // Identical op sequence: fill past capacity, then an oversized push.
    for (uint32_t i = 0; i < 5; i++) {
        bb_queue_push(legacy, "x", 1, (int64_t)i, i);
        bb_queue_push(ex,     "x", 1, (int64_t)i, i);
    }
    uint8_t big[32];
    memset(big, 0xAB, sizeof(big));
    bb_queue_push(legacy, big, sizeof(big), 5, 5);
    bb_queue_push(ex,     big, sizeof(big), 5, 5);

    TEST_ASSERT_EQUAL_size_t(bb_queue_count(legacy),     bb_queue_count(ex));
    TEST_ASSERT_EQUAL_size_t(bb_queue_bytes_used(legacy), bb_queue_bytes_used(ex));
    TEST_ASSERT_EQUAL_size_t(bb_queue_dropped(legacy),    bb_queue_dropped(ex));
    TEST_ASSERT_EQUAL_size_t(bb_queue_truncated(legacy),  bb_queue_truncated(ex));

    bb_queue_destroy(legacy);
    bb_queue_destroy(ex);
}

// ---------------------------------------------------------------------------
// Byte budget (max_bytes) — eviction / rejection (B1-1031)
// ---------------------------------------------------------------------------

void test_bb_queue_budget_evict_oldest(void)
{
    bb_queue_t r = make_ring_ex(8, 10, BB_QUEUE_EVICT_OLDEST, 10, 0);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "abcde", 5, 0, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "fghij", 5, 0, 2));
    TEST_ASSERT_EQUAL_size_t(10, bb_queue_bytes_used(r));

    // Pushing 1 more byte exceeds the 10-byte budget -- evicts entry 1.
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "k", 1, 0, 3));

    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(6, bb_queue_bytes_used(r));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_dropped(r));

    size_t out_len; int64_t out_ts; uint32_t out_id; uint8_t buf[32] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(2, out_id);

    bb_queue_destroy(r);
}

void test_bb_queue_budget_evict_multi_entry_to_fit(void)
{
    // Three 3-byte entries fill the 10-byte budget's usable region; a
    // single 8-byte push must evict ALL THREE (N>1 evictions) to fit.
    bb_queue_t r = make_ring_ex(10, 10, BB_QUEUE_EVICT_OLDEST, 10, 0);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "aaa", 3, 0, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "bbb", 3, 0, 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "ccc", 3, 0, 3));
    TEST_ASSERT_EQUAL_size_t(9, bb_queue_bytes_used(r));

    uint8_t big[8];
    memset(big, 0xEE, sizeof(big));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, big, sizeof(big), 0, 4));

    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(8, bb_queue_bytes_used(r));
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_dropped(r));

    size_t out_len; int64_t out_ts; uint32_t out_id; uint8_t buf[32] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(4, out_id);

    bb_queue_destroy(r);
}

void test_bb_queue_budget_reject_new(void)
{
    bb_queue_t r = make_ring_ex(8, 10, BB_QUEUE_REJECT_NEW, 10, 0);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "abc", 3, 0, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "defgh", 5, 0, 2));
    TEST_ASSERT_EQUAL_size_t(8, bb_queue_bytes_used(r));

    // Would push bytes_used to 13 > 10 -- rejected outright, no eviction.
    bb_err_t err = bb_queue_push(r, "jklmn", 5, 0, 3);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(8, bb_queue_bytes_used(r));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_dropped(r));

    // Oldest entry preserved intact.
    size_t out_len; int64_t out_ts; uint32_t out_id; uint8_t buf[32] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(1, out_id);

    bb_queue_destroy(r);
}

// NOTE: the former test_bb_queue_budget_reject_len_exceeds_max_bytes_outright
// (len alone > max_bytes while len <= max_entry_bytes) is no longer
// constructible: bb_queue_create_ex() now rejects any max_bytes < max_entry
// _bytes at config time (B1-1031 review, see
// test_bb_queue_create_ex_max_bytes_below_max_entry_rejected below), so a
// push can never reach a state where len exceeds max_bytes without having
// already tripped the max_entry_bytes truncate check first.

// ---------------------------------------------------------------------------
// Coalescing capacity-one ring under a byte-agnostic evict (sanity for the
// combined space check with capacity==1)
// ---------------------------------------------------------------------------

void test_bb_queue_coalesce_via_capacity_one(void)
{
    bb_queue_t r = make_ring_ex(1, 8, BB_QUEUE_EVICT_OLDEST, 0, 0);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "a", 1, 0, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "b", 1, 0, 2));

    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_dropped(r));

    size_t out_len; int64_t out_ts; uint32_t out_id; uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(2, out_id);
    TEST_ASSERT_EQUAL_STRING("b", (const char *)buf);

    bb_queue_destroy(r);
}

// ---------------------------------------------------------------------------
// Age eviction (max_age) — sweep-on-push + explicit sweep (B1-1031)
// ---------------------------------------------------------------------------

void test_bb_queue_age_evict_on_push(void)
{
    bb_queue_t r = make_ring_ex(8, 32, BB_QUEUE_EVICT_OLDEST, 0, 1000);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "a", 1, 0, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "b", 1, 500, 2));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r));

    // Pushing at ts=2000: entry 1 (age 2000) and entry 2 (age 1500) both
    // >= max_age=1000 -- both expire before this entry is written.
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "c", 1, 2000, 3));

    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_dropped(r));

    size_t out_len; int64_t out_ts; uint32_t out_id; uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(3, out_id);

    bb_queue_destroy(r);
}

void test_bb_queue_evict_expired_explicit(void)
{
    bb_queue_t r = make_ring_ex(8, 32, BB_QUEUE_EVICT_OLDEST, 0, 1000);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "a", 1, 0, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "b", 1, 500, 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "c", 1, 900, 3));
    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));

    // At now=1500: entry 1 (age 1500) and entry 2 (age 1000) expire;
    // entry 3 (age 600) does not -- PARTIAL sweep.
    size_t evicted = bb_queue_evict_expired(r, 1500);
    TEST_ASSERT_EQUAL_size_t(2, evicted);

    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_dropped(r));

    size_t out_len; int64_t out_ts; uint32_t out_id; uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_peek_oldest(r, buf, sizeof(buf), &out_len, &out_ts, &out_id));
    TEST_ASSERT_EQUAL_UINT32(3, out_id);

    bb_queue_destroy(r);
}

void test_bb_queue_evict_expired_noop_disabled(void)
{
    bb_queue_t r = make_ring(4, 8);  // max_age disabled (legacy ctor)
    bb_queue_push(r, "a", 1, 0, 1);

    TEST_ASSERT_EQUAL_size_t(0, bb_queue_evict_expired(r, 999999));
    TEST_ASSERT_EQUAL_size_t(1, bb_queue_count(r));

    bb_queue_destroy(r);
}

void test_bb_queue_evict_expired_noop_empty(void)
{
    bb_queue_t r = make_ring_ex(4, 8, BB_QUEUE_EVICT_OLDEST, 0, 1000);
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_evict_expired(r, 999999));
    bb_queue_destroy(r);
}

void test_bb_queue_evict_expired_null_ring_returns_zero(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_evict_expired(NULL, 0));
}

// ---------------------------------------------------------------------------
// Backward/out-of-order timestamps never purge fresh entries (HIGH finding)
// ---------------------------------------------------------------------------

void test_bb_queue_age_backward_ts_no_purge(void)
{
    // max_age=1000; ts deltas between pushes stay well under the eviction
    // threshold (500 < 1000) so the ordinary sweep-on-push doesn't evict
    // "a" while pushing "b" -- this test is isolating the BACKWARD-ts
    // clamp behavior, not the (already-covered) forward sweep-on-push path.
    bb_queue_t r = make_ring_ex(8, 32, BB_QUEUE_EVICT_OLDEST, 0, 1000);

    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "a", 1, 1000, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "b", 1, 1500, 2));

    // Backward push: ts=500 is earlier than both existing entries' ts. A
    // naive (unclamped) age computation would wrap negative to ~UINT64_MAX
    // and wrongly evict the fresh entries. Both must survive.
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r, "c", 1, 500, 3));

    TEST_ASSERT_EQUAL_size_t(3, bb_queue_count(r));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_dropped(r));

    // Direct explicit sweep with a backward `now` against ts={1000,1500}
    // must evict nothing.
    bb_queue_t r2 = make_ring_ex(8, 32, BB_QUEUE_EVICT_OLDEST, 0, 1000);
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r2, "x", 1, 1000, 10));
    TEST_ASSERT_EQUAL(BB_OK, bb_queue_push(r2, "y", 1, 1500, 20));

    TEST_ASSERT_EQUAL_size_t(0, bb_queue_evict_expired(r2, 500));
    TEST_ASSERT_EQUAL_size_t(2, bb_queue_count(r2));
    TEST_ASSERT_EQUAL_size_t(0, bb_queue_dropped(r2));

    bb_queue_destroy(r);
    bb_queue_destroy(r2);
}

// ---------------------------------------------------------------------------
// bb_queue_create_ex — max_bytes smaller than max_entry_bytes is rejected
// (LOW finding)
// ---------------------------------------------------------------------------

void test_bb_queue_create_ex_max_bytes_below_max_entry_rejected(void)
{
    bb_queue_cfg_t cfg = {
        .capacity_entries = 4,
        .max_entry_bytes  = 32,
        .policy           = BB_QUEUE_EVICT_OLDEST,
        .name             = "t",
        .max_bytes        = 16,  // smaller than max_entry_bytes -- never fits
        .max_age          = 0,
    };
    bb_queue_t r = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_queue_create_ex(&cfg, &r));
    TEST_ASSERT_NULL(r);
}
