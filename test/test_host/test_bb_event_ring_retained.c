#include "unity.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void setup_sync_mode(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
}

static bb_event_cfg_t small_cfg = {
    .queue_depth = 8,
    .max_payload = 256,
};

typedef struct {
    int call_count;
    struct {
        int32_t id;
        size_t  size;
        uint8_t payload[128];
    } calls[32];
} rlog_t;

static void rlog_record(bb_event_topic_t topic,
                        int32_t id,
                        const void *data, size_t size,
                        void *user)
{
    (void)topic;
    rlog_t *l = (rlog_t *)user;
    if (l->call_count < 32) {
        l->calls[l->call_count].id   = id;
        l->calls[l->call_count].size = size;
        if (size > 0 && data && size <= sizeof(l->calls[0].payload)) {
            memcpy(l->calls[l->call_count].payload, data, size);
        }
        l->call_count++;
    }
}

// ---------------------------------------------------------------------------
// attach_ex with retained=true: basic flag acceptance
// ---------------------------------------------------------------------------

void test_bb_event_ring_attach_ex_retained_true_returns_ok(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ret.test1", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach_ex(topic, 4, 64, true, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(ring);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_attach_ex_retained_false_same_as_attach(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ret.test2", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach_ex(topic, 4, 64, false, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(ring);

    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// retained ring: post once, subscribe — replay delivers the post
// ---------------------------------------------------------------------------

void test_bb_event_ring_retained_subscribe_after_one_post_replays(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ret.test3", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach_ex(topic, 4, 64, true, &ring);

    // Post exactly once
    bb_event_post(topic, 42, "state0", 7);
    bb_event_pump(0);

    // Subscribe with replay — must deliver the single post
    rlog_t log = {0};
    bb_event_sub_t sub = NULL;
    bb_err_t err = bb_event_ring_subscribe_with_replay(ring, rlog_record, &log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(42, log.calls[0].id);
    TEST_ASSERT_EQUAL_STRING("state0", (const char *)log.calls[0].payload);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// retained ring: capacity=1, post N+1 times — subscribe delivers most recent
// ---------------------------------------------------------------------------

void test_bb_event_ring_retained_capacity1_overflow_delivers_most_recent(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ret.test4", &topic);

    // capacity=1 retained ring
    bb_event_ring_t ring = NULL;
    bb_event_ring_attach_ex(topic, 1, 64, true, &ring);

    // Post 5 times (4 more than capacity)
    for (int i = 0; i < 5; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "state%d", i);
        bb_event_post(topic, 100 + i, buf, strlen(buf) + 1);
    }
    bb_event_pump(0);

    // Only 1 slot; ring holds the most recent entry (state4 / id=104)
    TEST_ASSERT_EQUAL(1, bb_event_ring_count(ring));

    rlog_t log = {0};
    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, rlog_record, &log, &sub);
    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(104, log.calls[0].id);
    TEST_ASSERT_EQUAL_STRING("state4", (const char *)log.calls[0].payload);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// retained ring: capacity=N, post N+1 — subscribe delivers N most-recent
// ---------------------------------------------------------------------------

void test_bb_event_ring_retained_overflow_delivers_most_recent_n(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ret.test5", &topic);

    const size_t cap = 3;
    bb_event_ring_t ring = NULL;
    bb_event_ring_attach_ex(topic, cap, 64, true, &ring);

    // Post cap+1 entries — oldest (id=200) should be evicted
    for (int i = 0; i < (int)(cap + 1); i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "s%d", i);
        bb_event_post(topic, 200 + i, buf, strlen(buf) + 1);
    }
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(cap, bb_event_ring_count(ring));

    rlog_t log = {0};
    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, rlog_record, &log, &sub);

    // Should have ids 201, 202, 203 (oldest 200 evicted)
    TEST_ASSERT_EQUAL((int)cap, log.call_count);
    TEST_ASSERT_EQUAL(201, log.calls[0].id);
    TEST_ASSERT_EQUAL(202, log.calls[1].id);
    TEST_ASSERT_EQUAL(203, log.calls[2].id);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// attach_ex null-arg guard
// ---------------------------------------------------------------------------

void test_bb_event_ring_attach_ex_null_topic_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach_ex(NULL, 4, 64, true, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_ring_attach_ex_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ret.nullout", &topic);

    bb_err_t err = bb_event_ring_attach_ex(topic, 4, 64, true, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}
