#include "unity.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include <stdlib.h>
#include <string.h>

static void setup_sync_mode(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
}

static bb_event_cfg_t small_cfg = {
    .queue_depth = 8,
    .max_payload = 256,
};

// ---------------------------------------------------------------------------
// bb_event_ring_capacity
// ---------------------------------------------------------------------------

void test_bb_event_ring_capacity_null_returns_zero(void)
{
    TEST_ASSERT_EQUAL(0, bb_event_ring_capacity(NULL));
}

void test_bb_event_ring_capacity_returns_configured_value(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("cap.test1", &topic);

    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_attach(topic, 7, 32, &ring));
    TEST_ASSERT_EQUAL(7, bb_event_ring_capacity(ring));

    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// bb_event_ring_count
// ---------------------------------------------------------------------------

void test_bb_event_ring_count_null_returns_zero(void)
{
    TEST_ASSERT_EQUAL(0, bb_event_ring_count(NULL));
}

void test_bb_event_ring_count_empty_ring_returns_zero(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("cnt.empty", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 4, 32, &ring);

    TEST_ASSERT_EQUAL(0, bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_count_after_posts(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("cnt.posts", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 8, 32, &ring);

    for (int i = 0; i < 3; i++) {
        bb_event_post(topic, i, "x", 2);
    }
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(3, bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_count_capped_at_capacity(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("cnt.cap", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 3, 32, &ring);

    for (int i = 0; i < 10; i++) {
        bb_event_post(topic, i, "y", 2);
    }
    bb_event_pump(0);

    // Count must not exceed capacity.
    TEST_ASSERT_EQUAL(3, bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// bb_event_ring_last_entry_info
// ---------------------------------------------------------------------------

void test_bb_event_ring_last_entry_info_null_ring_returns_invalid_arg(void)
{
    uint32_t id = 0;
    size_t   sz = 0;
    int64_t  us = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_event_ring_last_entry_info(NULL, &id, &sz, &us));
}

void test_bb_event_ring_last_entry_info_empty_ring_returns_not_found(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lei.empty", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 4, 32, &ring);

    uint32_t id = 99;
    size_t   sz = 99;
    int64_t  us = 99;
    bb_err_t err = bb_event_ring_last_entry_info(ring, &id, &sz, &us);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
    // Out-params untouched on NOT_FOUND.
    TEST_ASSERT_EQUAL(99, id);
    TEST_ASSERT_EQUAL(99, sz);
    TEST_ASSERT_EQUAL(99, us);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_last_entry_info_populated_ring(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lei.pop", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 4, 64, &ring);

    const char *payload = "{\"v\":42}";
    size_t plen = strlen(payload);
    bb_event_post(topic, 123, payload, plen);
    bb_event_pump(0);

    uint32_t id = 0;
    size_t   sz = 0;
    int64_t  us = 0;
    bb_err_t err = bb_event_ring_last_entry_info(ring, &id, &sz, &us);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(123, id);
    TEST_ASSERT_EQUAL(plen, sz);
    TEST_ASSERT_TRUE(us > 0);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_last_entry_info_reflects_latest_post(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lei.latest", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 4, 64, &ring);

    bb_event_post(topic, 1, "a", 2);
    bb_event_post(topic, 2, "bb", 3);
    bb_event_post(topic, 3, "ccc", 4);
    bb_event_pump(0);

    uint32_t id = 0;
    size_t   sz = 0;
    int64_t  us = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_last_entry_info(ring, &id, &sz, &us));
    // Most recent post was id=3, size=4.
    TEST_ASSERT_EQUAL(3, id);
    TEST_ASSERT_EQUAL(4, sz);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_last_entry_info_after_eviction(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lei.evict", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 2, 32, &ring);

    // Post 5 events into a capacity-2 ring.
    for (int i = 1; i <= 5; i++) {
        bb_event_post(topic, i, "x", 2);
    }
    bb_event_pump(0);

    uint32_t id = 0;
    size_t   sz = 0;
    int64_t  us = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_last_entry_info(ring, &id, &sz, &us));
    // Last post was id=5.
    TEST_ASSERT_EQUAL(5, id);
    TEST_ASSERT_EQUAL(2, sz);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_last_entry_info_null_out_params_ok(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lei.null_out", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 4, 32, &ring);

    bb_event_post(topic, 7, "hi", 3);
    bb_event_pump(0);

    // All out-params NULL — should not crash.
    bb_err_t err = bb_event_ring_last_entry_info(ring, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_last_entry_info_zero_size_payload(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lei.zerosz", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 4, 32, &ring);

    bb_event_post(topic, 55, NULL, 0);
    bb_event_pump(0);

    uint32_t id = 0;
    size_t   sz = 99;
    int64_t  us = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_last_entry_info(ring, &id, &sz, &us));
    TEST_ASSERT_EQUAL(55, id);
    TEST_ASSERT_EQUAL(0, sz);

    bb_event_ring_detach(ring);
}
