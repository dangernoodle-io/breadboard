// Tests for bb_bqueue: blocking mailbox (capacity==1) / bounded-MPSC
// (capacity>1) queue. Host backend uses REAL cross-thread pthread blocking
// (bb_lock_t + bb_lock_cond_t) -- every blocking scenario below genuinely
// parks a thread and wakes it from another, using bb_bqueue_test_waiting_count()
// as a sleep-free release gate (mirrors test_bb_lock_cond.c's
// registered[]-under-the-mutex idiom, adapted to bb_bqueue's opaque handle).
#include "unity.h"
#include "bb_bqueue.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>

static void reset_world(void)
{
    bb_bqueue_test_reset();
}

static bb_bqueue_t make_mailbox(void)
{
    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = sizeof(uint32_t), .name = "mbox" };
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_create(&cfg, &q));
    TEST_ASSERT_NOT_NULL(q);
    return q;
}

static bb_bqueue_t make_mpsc(size_t capacity)
{
    bb_bqueue_cfg_t cfg = { .capacity = capacity, .item_bytes = sizeof(uint32_t), .name = "mpsc" };
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_create(&cfg, &q));
    TEST_ASSERT_NOT_NULL(q);
    return q;
}

static void wait_for_waiting_count(bb_bqueue_t q, size_t n)
{
    while (bb_bqueue_test_waiting_count(q) < n) {
        sched_yield();
    }
}

// ---------------------------------------------------------------------------
// create() / destroy() -- validation, pool exhaustion
// ---------------------------------------------------------------------------

void test_bb_bqueue_create_null_cfg_returns_invalid_arg(void)
{
    reset_world();
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_create(NULL, &q));
}

void test_bb_bqueue_create_null_out_returns_invalid_arg(void)
{
    reset_world();
    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 4 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_create(&cfg, NULL));
}

void test_bb_bqueue_create_zero_capacity_returns_invalid_arg(void)
{
    reset_world();
    bb_bqueue_cfg_t cfg = { .capacity = 0, .item_bytes = 4 };
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_create(&cfg, &q));
}

void test_bb_bqueue_create_zero_item_bytes_returns_invalid_arg(void)
{
    reset_world();
    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 0 };
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_create(&cfg, &q));
}

// BB_BQUEUE_MAX_CAPACITY/BB_BQUEUE_MAX_ITEM_BYTES are compiler command-line
// macros (platformio.ini's [env:native] build_flags), visible in every TU
// compiled under this env without needing bb_bqueue_priv.h's own
// Kconfig-bridge #ifndef fallbacks (those only fire when the macro is NOT
// already defined, i.e. on a real ESP-IDF/Kconfig build).
void test_bb_bqueue_create_capacity_over_kconfig_max_returns_invalid_arg(void)
{
    reset_world();
    bb_bqueue_cfg_t cfg = { .capacity = BB_BQUEUE_MAX_CAPACITY + 1, .item_bytes = 4 };
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_create(&cfg, &q));
}

void test_bb_bqueue_create_item_bytes_over_kconfig_max_returns_invalid_arg(void)
{
    reset_world();
    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = BB_BQUEUE_MAX_ITEM_BYTES + 1 };
    bb_bqueue_t q = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_create(&cfg, &q));
}

void test_bb_bqueue_create_pool_exhausted_returns_no_space(void)
{
    reset_world();
    // BB_BQUEUE_MAX_INSTANCES is the Kconfig default (2) in this test env
    // (native's build_flags override capacity/item_bytes, not instances --
    // see platformio.ini). Exhaust the pool, then prove the NEXT create()
    // fails, then free one and prove a slot frees up again.
    bb_bqueue_t handles[8];
    int created = 0;
    while (created < 8) {
        bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 4 };
        bb_bqueue_t q = NULL;
        bb_err_t rc = bb_bqueue_create(&cfg, &q);
        if (rc != BB_OK) {
            TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
            break;
        }
        handles[created++] = q;
    }
    TEST_ASSERT_TRUE(created > 0);

    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 4 };
    bb_bqueue_t overflow = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_bqueue_create(&cfg, &overflow));

    bb_bqueue_destroy(handles[0]);
    bb_bqueue_t reclaimed = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_create(&cfg, &reclaimed));
    TEST_ASSERT_NOT_NULL(reclaimed);
}

void test_bb_bqueue_destroy_null_is_noop(void)
{
    reset_world();
    bb_bqueue_destroy(NULL); // must not crash
}

void test_bb_bqueue_operations_on_destroyed_handle_return_invalid_arg_or_zero(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();
    uint32_t v = 1, out = 0;
    bb_bqueue_destroy(q);

    // q's memory is still a valid pointer (static pool) but in_use=false --
    // exercises inst_from_handle()'s "stale handle" branch, distinct from
    // the NULL-handle branch covered by the null-arg-guard tests above.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_overwrite(q, &v));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_reset(q));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_peek(q, &out, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_receive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_count(q));
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_capacity(q));
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_test_waiting_count(q));
}

void test_bb_bqueue_test_waiting_count_null_handle_returns_zero(void)
{
    reset_world();
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_test_waiting_count(NULL));
}

// ---------------------------------------------------------------------------
// Mode enforcement -- the mode restriction is CHECKED, not just documented.
// ---------------------------------------------------------------------------

void test_bb_bqueue_overwrite_on_mpsc_queue_returns_invalid_state(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(4);
    uint32_t v = 1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_bqueue_overwrite(q, &v));
}

void test_bb_bqueue_reset_on_mpsc_queue_returns_invalid_state(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(4);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_bqueue_reset(q));
}

void test_bb_bqueue_send_on_mailbox_queue_returns_invalid_state(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();
    uint32_t v = 1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_bqueue_send(q, &v, 0));
}

void test_bb_bqueue_dropped_on_mailbox_queue_returns_invalid_state(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();
    size_t dropped = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_bqueue_dropped(q, &dropped));
}

// ---------------------------------------------------------------------------
// NULL-arg guards
// ---------------------------------------------------------------------------

void test_bb_bqueue_overwrite_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();
    uint32_t v = 1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_overwrite(NULL, &v));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_overwrite(q, NULL));
}

void test_bb_bqueue_reset_null_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_reset(NULL));
}

void test_bb_bqueue_send_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(4);
    uint32_t v = 1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_send(NULL, &v, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_send(q, NULL, 0));
}

void test_bb_bqueue_dropped_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(4);
    size_t dropped = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_dropped(NULL, &dropped));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_dropped(q, NULL));
}

void test_bb_bqueue_peek_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();
    uint32_t v;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_peek(NULL, &v, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_peek(q, NULL, 0));
}

void test_bb_bqueue_receive_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();
    uint32_t v;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_receive(NULL, &v, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_receive(q, NULL, 0));
}

void test_bb_bqueue_count_capacity_null_handle_return_zero(void)
{
    reset_world();
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_count(NULL));
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_capacity(NULL));
}

// ---------------------------------------------------------------------------
// 1. Mailbox overwrite (A then B -> peek returns B); reset -> peek(0) = NOT_FOUND.
// ---------------------------------------------------------------------------

void test_bb_bqueue_mailbox_overwrite_then_peek_returns_latest(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();

    uint32_t a = 1, b = 2, out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_overwrite(q, &a));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_overwrite(q, &b));

    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_peek(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(b, out);
    TEST_ASSERT_EQUAL_UINT(1, bb_bqueue_count(q));
    TEST_ASSERT_EQUAL_UINT(1, bb_bqueue_capacity(q));
}

void test_bb_bqueue_mailbox_reset_then_peek_zero_timeout_returns_not_found(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();

    uint32_t a = 42, out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_overwrite(q, &a));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_reset(q));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_bqueue_peek(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT(0, bb_bqueue_count(q));
}

void test_bb_bqueue_mailbox_receive_consumes_then_peek_returns_not_found(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();

    uint32_t a = 7, out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_overwrite(q, &a));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_receive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(a, out);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_bqueue_peek(q, &out, 0));
}

void test_bb_bqueue_peek_does_not_consume(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();

    uint32_t a = 9, out1 = 0, out2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_overwrite(q, &a));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_peek(q, &out1, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_peek(q, &out2, 0));
    TEST_ASSERT_EQUAL_UINT32(a, out1);
    TEST_ASSERT_EQUAL_UINT32(a, out2);
    TEST_ASSERT_EQUAL_UINT(1, bb_bqueue_count(q));
}

// ---------------------------------------------------------------------------
// 2. TWO CONCURRENT PEEKERS + one writer -- the property that protects
// TaipanMiner's work_queue from a dropped-mining-work regression. Both
// threads block in peek(), barrier-synchronized via
// bb_bqueue_test_waiting_count() (NOT a sleep) so BOTH are genuinely parked
// before overwrite() runs; a bounded (not literally infinite) timeout is
// used so that a signal()-instead-of-broadcast() regression FAILS this test
// via a clean BB_ERR_TIMEOUT assertion rather than hanging the test binary
// forever -- see the manual signal()-substitution non-vacuity proof in the
// PR description.
// ---------------------------------------------------------------------------

#define BB_BQUEUE_TEST_PEEK_TIMEOUT_MS 5000u

typedef struct {
    bb_bqueue_t q;
    bb_err_t    result;
    uint32_t    value;
} bb_bqueue_test_peeker_ctx_t;

static void *bb_bqueue_test_peeker_fn(void *arg)
{
    bb_bqueue_test_peeker_ctx_t *ctx = (bb_bqueue_test_peeker_ctx_t *)arg;
    ctx->result = bb_bqueue_peek(ctx->q, &ctx->value, BB_BQUEUE_TEST_PEEK_TIMEOUT_MS);
    return NULL;
}

void test_bb_bqueue_two_concurrent_peekers_both_woken_by_overwrite(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();

    bb_bqueue_test_peeker_ctx_t ctxs[2] = {
        { .q = q, .result = BB_ERR_INVALID_STATE, .value = 0 },
        { .q = q, .result = BB_ERR_INVALID_STATE, .value = 0 },
    };
    pthread_t threads[2];
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[0], NULL, bb_bqueue_test_peeker_fn, &ctxs[0]));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[1], NULL, bb_bqueue_test_peeker_fn, &ctxs[1]));

    // Genuinely parked -- not merely "thread started" -- both must have
    // reached bb_lock_cond_wait() before we write.
    wait_for_waiting_count(q, 2);

    uint32_t v = 123;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_overwrite(q, &v));

    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    TEST_ASSERT_EQUAL(BB_OK, ctxs[0].result);
    TEST_ASSERT_EQUAL(BB_OK, ctxs[1].result);
    TEST_ASSERT_EQUAL_UINT32(v, ctxs[0].value);
    TEST_ASSERT_EQUAL_UINT32(v, ctxs[1].value);
}

// ---------------------------------------------------------------------------
// 3. Timeout actually elapses on an empty mailbox.
// ---------------------------------------------------------------------------

void test_bb_bqueue_peek_timeout_elapses_on_empty_mailbox(void)
{
    reset_world();
    bb_bqueue_t q = make_mailbox();

    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_bqueue_peek(q, &out, 50));
}

// ---------------------------------------------------------------------------
// 4. MPSC blocking send: fill to capacity, producer blocks in send(FOREVER),
// consumer receive()s, producer unblocks.
// ---------------------------------------------------------------------------

typedef struct {
    bb_bqueue_t q;
    uint32_t    value;
    bb_err_t    result;
} bb_bqueue_test_sender_ctx_t;

static void *bb_bqueue_test_sender_fn(void *arg)
{
    bb_bqueue_test_sender_ctx_t *ctx = (bb_bqueue_test_sender_ctx_t *)arg;
    ctx->result = bb_bqueue_send(ctx->q, &ctx->value, BB_BQUEUE_WAIT_FOREVER);
    return NULL;
}

void test_bb_bqueue_mpsc_send_blocks_until_receive_frees_slot(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(2);

    uint32_t a = 1, b = 2, c = 3;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &a, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &b, 0));
    TEST_ASSERT_EQUAL_UINT(2, bb_bqueue_count(q));

    bb_bqueue_test_sender_ctx_t ctx = { .q = q, .value = c, .result = BB_ERR_INVALID_STATE };
    pthread_t sender;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&sender, NULL, bb_bqueue_test_sender_fn, &ctx));

    wait_for_waiting_count(q, 1);

    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_receive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(a, out);

    pthread_join(sender, NULL);
    TEST_ASSERT_EQUAL(BB_OK, ctx.result);
    TEST_ASSERT_EQUAL_UINT(2, bb_bqueue_count(q));
}

// ---------------------------------------------------------------------------
// 5. MPSC drop-on-full: send(item, 0) at capacity -> BB_ERR_NO_SPACE,
// dropped() increments.
// ---------------------------------------------------------------------------

void test_bb_bqueue_mpsc_send_zero_timeout_at_capacity_drops(void)
{
    reset_world();
    // capacity=2 -- capacity=1 would be mailbox mode (mode enforcement
    // would reject send() outright); MPSC requires capacity > 1.
    bb_bqueue_t q = make_mpsc(2);

    uint32_t a = 1, b = 2, c = 3;
    size_t dropped = 999;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &a, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &b, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_dropped(q, &dropped));
    TEST_ASSERT_EQUAL_UINT(0, dropped);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_bqueue_send(q, &c, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_dropped(q, &dropped));
    TEST_ASSERT_EQUAL_UINT(1, dropped);
}

void test_bb_bqueue_mpsc_send_timeout_at_capacity_drops(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(2);

    uint32_t a = 1, b = 2, c = 3;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &a, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &b, 0));
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_bqueue_send(q, &c, 50));

    size_t dropped = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_dropped(q, &dropped));
    TEST_ASSERT_EQUAL_UINT(1, dropped);
}

// ---------------------------------------------------------------------------
// MPSC receive on empty: timeout / zero-timeout NOT_FOUND.
// ---------------------------------------------------------------------------

void test_bb_bqueue_mpsc_receive_zero_timeout_empty_returns_not_found(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(2);
    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_bqueue_receive(q, &out, 0));
}

void test_bb_bqueue_mpsc_receive_timeout_elapses_on_empty(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(2);
    uint32_t out = 0;
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_bqueue_receive(q, &out, 50));
}

void test_bb_bqueue_mpsc_fifo_order_preserved(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(3);

    uint32_t a = 1, b = 2, c = 3, out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &a, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &b, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_send(q, &c, 0));

    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_receive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(a, out);
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_receive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(b, out);
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_receive(q, &out, 0));
    TEST_ASSERT_EQUAL_UINT32(c, out);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_bqueue_receive(q, &out, 0));
}

// ---------------------------------------------------------------------------
// 7. TWO CONCURRENT PRODUCERS + one consumer -- the property that protects
// TaipanMiner's result_queue (mining.c:806 + asic_task.c:862, two genuine
// producer call sites into a single MPSC bb_bqueue). dropped()/count()
// accounting is currently correct only because every send()/dropped-read
// sits under one `inst->lock`; this test cross-checks bb_bqueue_dropped()
// against each producer's own locally-counted rejections so a later
// refactor narrowing that lock's scope (e.g. moving `dropped++` or the
// ring-buffer memcpy outside it) would show up as a mismatched count or a
// corrupted/duplicated/lost item, not just a hang.
//
// Cross-producer interleaving order is UNSPECIFIED and intentionally not
// asserted; only FIFO order *within* each producer's own successfully-sent
// items is checked (guaranteed because a producer's own sends are issued
// sequentially by its single thread, and the shared lock serializes their
// insertion into the ring buffer in that same relative order).
// ---------------------------------------------------------------------------

#define BB_BQUEUE_TEST_MPSC_PRODUCERS         2u
#define BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER 200u
#define BB_BQUEUE_TEST_MPSC_SEQ_BITS           24u
#define BB_BQUEUE_TEST_MPSC_SEQ_MASK           ((1u << BB_BQUEUE_TEST_MPSC_SEQ_BITS) - 1u)

typedef struct {
    bb_bqueue_t q;
    uint32_t    producer_id;
    bool        sent_ok[BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER];
    size_t      dropped_local;
} bb_bqueue_test_mpsc_producer_ctx_t;

static void *bb_bqueue_test_mpsc_producer_fn(void *arg)
{
    bb_bqueue_test_mpsc_producer_ctx_t *ctx = (bb_bqueue_test_mpsc_producer_ctx_t *)arg;
    for (uint32_t seq = 0; seq < BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER; seq++) {
        uint32_t value = (ctx->producer_id << BB_BQUEUE_TEST_MPSC_SEQ_BITS) | seq;
        bb_err_t rc = bb_bqueue_send(ctx->q, &value, 0);
        if (rc == BB_OK) {
            ctx->sent_ok[seq] = true;
        } else {
            TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
            ctx->dropped_local++;
        }
        sched_yield(); // encourage cross-thread interleaving
    }
    return NULL;
}

typedef struct {
    bb_bqueue_t q;
    uint32_t    received[BB_BQUEUE_TEST_MPSC_PRODUCERS * BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER];
    size_t      received_count;
    volatile bool stop;
} bb_bqueue_test_mpsc_consumer_ctx_t;

static void *bb_bqueue_test_mpsc_consumer_fn(void *arg)
{
    bb_bqueue_test_mpsc_consumer_ctx_t *ctx = (bb_bqueue_test_mpsc_consumer_ctx_t *)arg;
    for (;;) {
        uint32_t value = 0;
        bb_err_t rc = bb_bqueue_receive(ctx->q, &value, 20);
        if (rc == BB_OK) {
            ctx->received[ctx->received_count++] = value;
        } else if (ctx->stop) {
            break;
        }
    }
    return NULL;
}

void test_bb_bqueue_mpsc_two_producers_one_consumer_no_corruption(void)
{
    reset_world();
    bb_bqueue_t q = make_mpsc(2); // small capacity forces genuine contention/drops

    bb_bqueue_test_mpsc_producer_ctx_t producers[BB_BQUEUE_TEST_MPSC_PRODUCERS] = {
        { .q = q, .producer_id = 0 },
        { .q = q, .producer_id = 1 },
    };
    bb_bqueue_test_mpsc_consumer_ctx_t consumer = { .q = q };

    pthread_t consumer_thread;
    pthread_t producer_threads[BB_BQUEUE_TEST_MPSC_PRODUCERS];
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&consumer_thread, NULL, bb_bqueue_test_mpsc_consumer_fn, &consumer));
    for (uint32_t i = 0; i < BB_BQUEUE_TEST_MPSC_PRODUCERS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&producer_threads[i], NULL, bb_bqueue_test_mpsc_producer_fn, &producers[i]));
    }
    for (uint32_t i = 0; i < BB_BQUEUE_TEST_MPSC_PRODUCERS; i++) {
        pthread_join(producer_threads[i], NULL);
    }
    consumer.stop = true;
    pthread_join(consumer_thread, NULL);

    // dropped() equals exactly the number of genuinely-rejected sends,
    // summed across both producers.
    size_t dropped = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_dropped(q, &dropped));
    size_t expected_dropped = producers[0].dropped_local + producers[1].dropped_local;
    TEST_ASSERT_EQUAL_UINT(expected_dropped, dropped);

    size_t expected_received =
        (BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER - producers[0].dropped_local) +
        (BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER - producers[1].dropped_local);
    TEST_ASSERT_EQUAL_UINT(expected_received, consumer.received_count);

    // No corruption, no duplication, FIFO within each producer's own items.
    bool seen[BB_BQUEUE_TEST_MPSC_PRODUCERS][BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER];
    memset(seen, 0, sizeof(seen));
    uint32_t last_seq[BB_BQUEUE_TEST_MPSC_PRODUCERS] = { 0 };
    bool have_last[BB_BQUEUE_TEST_MPSC_PRODUCERS] = { false, false };

    for (size_t i = 0; i < consumer.received_count; i++) {
        uint32_t value = consumer.received[i];
        uint32_t pid = value >> BB_BQUEUE_TEST_MPSC_SEQ_BITS;
        uint32_t seq = value & BB_BQUEUE_TEST_MPSC_SEQ_MASK;
        TEST_ASSERT_TRUE(pid < BB_BQUEUE_TEST_MPSC_PRODUCERS);
        TEST_ASSERT_TRUE(seq < BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER);
        TEST_ASSERT_TRUE(producers[pid].sent_ok[seq]); // not corrupted into a bogus (producer, seq) pair
        TEST_ASSERT_FALSE(seen[pid][seq]);              // not duplicated
        seen[pid][seq] = true;

        if (have_last[pid]) {
            TEST_ASSERT_TRUE(seq > last_seq[pid]); // FIFO within this producer's own items
        }
        last_seq[pid] = seq;
        have_last[pid] = true;
    }

    for (uint32_t pid = 0; pid < BB_BQUEUE_TEST_MPSC_PRODUCERS; pid++) {
        for (uint32_t seq = 0; seq < BB_BQUEUE_TEST_MPSC_ITEMS_PER_PRODUCER; seq++) {
            if (producers[pid].sent_ok[seq]) {
                TEST_ASSERT_TRUE(seen[pid][seq]); // no loss
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 8. Pure deadline helper -- exhaustive branch coverage. Declared in the
// private header (components/bb_bqueue/src/bb_bqueue_priv.h), which is on
// this native env's include path (PRIV_INCLUDE_DIRS "src" -- same
// convention test_bb_lock_cond_waiterlist.c relies on for bb_core's own
// private header).
// ---------------------------------------------------------------------------

#include "bb_bqueue_priv.h"

void test_bb_bqueue_deadline_compute_adds_timeout_in_microseconds(void)
{
    TEST_ASSERT_EQUAL_UINT64(1000000ULL + 250000ULL, bb_bqueue_deadline_compute(1000000ULL, 250));
}

void test_bb_bqueue_deadline_remaining_ms_not_yet_expired(void)
{
    uint32_t remaining = 0;
    // deadline = 10_000_000us; now = 9_000_000us -> 1_000_000us = 1000ms remaining.
    TEST_ASSERT_TRUE(bb_bqueue_deadline_remaining_ms(10000000ULL, 9000000ULL, &remaining));
    TEST_ASSERT_EQUAL_UINT32(1000, remaining);
}

void test_bb_bqueue_deadline_remaining_ms_exactly_at_deadline_is_expired(void)
{
    uint32_t remaining = 0;
    TEST_ASSERT_FALSE(bb_bqueue_deadline_remaining_ms(5000ULL, 5000ULL, &remaining));
}

void test_bb_bqueue_deadline_remaining_ms_past_deadline_is_expired(void)
{
    uint32_t remaining = 0;
    TEST_ASSERT_FALSE(bb_bqueue_deadline_remaining_ms(5000ULL, 6000ULL, &remaining));
}

void test_bb_bqueue_validate_cfg_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_bqueue_validate_cfg(NULL));
}

void test_bb_bqueue_validate_cfg_valid_returns_ok(void)
{
    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 4 };
    TEST_ASSERT_EQUAL(BB_OK, bb_bqueue_validate_cfg(&cfg));
}
