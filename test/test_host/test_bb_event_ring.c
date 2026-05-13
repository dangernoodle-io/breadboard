#include "unity.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include "../../components/bb_event_ring/bb_event_ring_internal.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void setup_sync_mode(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
}

static bb_event_cfg_t small_cfg = {
    .queue_depth = 8,
    .max_payload = 256,
};

// Callback recorder for replayed and live events
#define MAX_HANDLER_CALLS 32

typedef struct {
    int call_count;
    struct {
        bb_event_topic_t topic;
        int32_t id;
        size_t size;
        uint8_t payload[128];
    } calls[MAX_HANDLER_CALLS];
} handler_log_t;

static void record_handler(bb_event_topic_t topic,
                          int32_t id,
                          const void *data, size_t size,
                          void *user)
{
    handler_log_t *log = (handler_log_t *)user;
    if (log->call_count < MAX_HANDLER_CALLS) {
        log->calls[log->call_count].topic = topic;
        log->calls[log->call_count].id = id;
        log->calls[log->call_count].size = size;
        if (size > 0 && data && size <= sizeof(log->calls[0].payload)) {
            memcpy(log->calls[log->call_count].payload, data, size);
        }
        log->call_count++;
    }
}

static void reset_log(handler_log_t *log)
{
    memset(log, 0, sizeof(*log));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_event_ring_attach_and_post_replay_delivers_all_entries(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test1", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 5, 64, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(ring);

    // Post N events before subscribing
    for (int i = 0; i < 3; i++) {
        char payload[16];
        snprintf(payload, sizeof(payload), "evt%d", i);
        bb_event_post(topic, 100 + i, payload, strlen(payload) + 1);
    }
    bb_event_pump(0);  // Drain the queue so ring captures

    // Now subscribe with replay
    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    err = bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Handler should have been called 3 times during replay
    TEST_ASSERT_EQUAL(3, log.call_count);
    TEST_ASSERT_EQUAL(100, log.calls[0].id);
    TEST_ASSERT_EQUAL(101, log.calls[1].id);
    TEST_ASSERT_EQUAL(102, log.calls[2].id);
    TEST_ASSERT_EQUAL_STRING("evt0", (const char *)log.calls[0].payload);
    TEST_ASSERT_EQUAL_STRING("evt1", (const char *)log.calls[1].payload);
    TEST_ASSERT_EQUAL_STRING("evt2", (const char *)log.calls[2].payload);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_bb_event_ring_capacity_overflow_evicts_oldest(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test2", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 3, 64, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Post 5 events (capacity is 3, so oldest 2 are evicted, keeping [1,2,3])
    for (int i = 0; i < 5; i++) {
        bb_event_post(topic, 200 + i, NULL, 0);
    }
    bb_event_pump(0);

    // Subscribe and replay
    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);

    // Last 3 events should remain: 202, 203, 204 (oldest 2 of 5 are evicted)
    TEST_ASSERT_EQUAL(3, log.call_count);
    TEST_ASSERT_EQUAL(202, log.calls[0].id);
    TEST_ASSERT_EQUAL(203, log.calls[1].id);
    TEST_ASSERT_EQUAL(204, log.calls[2].id);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_bb_event_ring_live_events_fire_after_subscribe(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test3", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    // Post one event before subscribe
    bb_event_post(topic, 300, "pre", 4);
    bb_event_pump(0);

    // Subscribe (replays 1 event)
    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(1, log.call_count);

    // Post new event after subscribe
    reset_log(&log);
    bb_event_post(topic, 301, "live", 5);
    bb_event_pump(0);

    // Should receive the live event
    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(301, log.calls[0].id);
    TEST_ASSERT_EQUAL_STRING("live", (const char *)log.calls[0].payload);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_bb_event_ring_detach_stops_capturing(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test4", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    // Post one event
    bb_event_post(topic, 400, "first", 6);
    bb_event_pump(0);

    // Detach
    bb_event_ring_detach(ring);

    // Create a new ring and attach to same topic
    bb_event_topic_t topic2 = NULL;
    bb_event_topic_lookup("ring.test4", &topic2);
    TEST_ASSERT_EQUAL_PTR(topic, topic2);

    bb_event_ring_t ring2 = NULL;
    bb_event_ring_attach(topic2, 5, 64, &ring2);

    // Post event to the old ring (should not be captured)
    bb_event_post(topic, 401, "second", 7);
    bb_event_pump(0);

    // Subscribe to new ring and check replay
    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring2, record_handler, &log, &sub);

    // Should only see the event posted after new ring was attached
    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(401, log.calls[0].id);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring2);
}

void test_bb_event_ring_payload_integrity(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test5", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    // Create test struct
    struct {
        uint32_t magic;
        uint16_t value;
        uint8_t arr[5];
    } data;
    data.magic = 0xdeadbeef;
    data.value = 0x1234;
    memset(data.arr, 0xAB, sizeof data.arr);

    bb_event_post(topic, 500, &data, sizeof data);
    bb_event_pump(0);

    // Replay and check
    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);

    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(sizeof data, log.calls[0].size);
    TEST_ASSERT_EQUAL_MEMORY(&data, log.calls[0].payload, sizeof data);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// Additional tests for branch coverage
// ---------------------------------------------------------------------------

void test_bb_event_ring_attach_null_topic_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(NULL, 5, 64, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_ring_attach_zero_capacity_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("dup.test", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 0, 64, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_ring_attach_zero_max_entry_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("lookup.test", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 5, 0, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_ring_attach_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("topic.cross.single", &topic);

    bb_err_t err = bb_event_ring_attach(topic, 5, 64, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_ring_subscribe_null_ring_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_err_t err = bb_event_ring_subscribe_with_replay(NULL, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_ring_subscribe_null_callback_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test1", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_err_t err = bb_event_ring_subscribe_with_replay(ring, NULL, &log, &sub);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_subscribe_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test2", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    handler_log_t log;
    reset_log(&log);

    bb_err_t err = bb_event_ring_subscribe_with_replay(ring, record_handler, &log, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_detach_null_noop(void)
{
    setup_sync_mode();
    // Should not crash (exercises line 192 NULL check)
    bb_event_ring_detach(NULL);
}

void test_bb_event_ring_head_wraps_modulo_capacity(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.test3", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 3, 64, &ring);

    // Post 7 events (wraps, exercises line 61 tail advance when count==capacity)
    for (int i = 0; i < 7; i++) {
        bb_event_post(topic, 700 + i, NULL, 0);
    }
    bb_event_pump(0);

    bb_event_ring_detach(ring);
}

void test_bb_event_ring_zero_payload_capture(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.zeropay", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    // Post zero-payload event (line 52 size > 0 false branch)
    bb_event_post(topic, 800, NULL, 0);
    bb_event_pump(0);

    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);

    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(0, log.calls[0].size);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_bb_event_ring_empty_ring_replay(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.emptyring", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 64, &ring);

    // Subscribe without posting anything (line 148 count > 0 false branch)
    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_err_t err = bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, log.call_count);  // No replay

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_bb_event_ring_payload_with_data(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("ring.payload2", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach(topic, 5, 128, &ring);

    // Post event with payload (line 52 size > 0 && data true branch)
    uint8_t payload[64];
    memset(payload, 0xAB, sizeof payload);
    bb_event_post(topic, 801, payload, sizeof payload);
    bb_event_pump(0);

    handler_log_t log;
    reset_log(&log);

    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);

    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(sizeof payload, log.calls[0].size);
    TEST_ASSERT_EQUAL_MEMORY(payload, log.calls[0].payload, sizeof payload);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_ring_capture_with_size_nonzero_data_null(void)
{
    // Post to ring-attached topic with data=NULL, size=8
    // Verify ring captures with size=8 but no payload bytes (tests line 52 path)
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.ring_null_data", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 4, 64, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_post(topic, 42, NULL, 8);
    bb_event_pump(0);

    handler_log_t log = {0};
    bb_event_sub_t sub = NULL;
    err = bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, log.call_count);
    TEST_ASSERT_EQUAL(8, log.calls[0].size);

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

void test_ring_attach_struct_calloc_fails(void)
{
    // Set ring allocator to fail on 1st calloc
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.ring_alloc_fail1", &topic);

    bb_event_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 0;  // Fail on first calloc (struct)

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 4, 64, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(ring);

    bb_event_ring_set_allocator(NULL, NULL);
}

void test_ring_attach_entries_calloc_fails(void)
{
    // Fail on 2nd calloc (entries array)
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.ring_alloc_fail2", &topic);

    bb_event_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 1;  // Fail on second calloc

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 4, 64, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(ring);

    bb_event_ring_set_allocator(NULL, NULL);
}

void test_ring_attach_payload_calloc_fails(void)
{
    // Fail on 3rd calloc (payload buffer)
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.ring_alloc_fail3", &topic);

    bb_event_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 2;  // Fail on third calloc

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 4, 64, &ring);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_NULL(ring);

    bb_event_ring_set_allocator(NULL, NULL);
}

void test_ring_subscribe_with_replay_snapshot_calloc_fails(void)
{
    // Attach ring with entries, post events, then fail calloc for snapshot
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.ring_replay_fail", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 4, 64, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Post some events
    bb_event_post(topic, 1, "data1", 6);
    bb_event_post(topic, 2, "data2", 6);
    bb_event_pump(0);

    // Now fail calloc for snapshot
    bb_event_ring_set_allocator(test_failing_calloc, free);
    test_alloc_fail_at = 0;

    handler_log_t log = {0};
    bb_event_sub_t sub = NULL;
    err = bb_event_ring_subscribe_with_replay(ring, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    bb_event_ring_set_allocator(NULL, NULL);
    bb_event_ring_detach(ring);
}

void test_ring_subscribe_when_subscriber_pool_exhausted(void)
{
    // Fill subscriber pool, then try to ring_subscribe_with_replay
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.ring_pool_exhausted", &topic);

    bb_event_ring_t ring = NULL;
    bb_err_t err = bb_event_ring_attach(topic, 4, 64, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Subscribe many times until pool is exhausted
    // CONFIG_BB_EVENT_MAX_SUBSCRIBERS is typically large, so we'll fill it
    bb_event_sub_t subs[512];
    int i = 0;
    for (i = 0; i < 512; i++) {
        handler_log_t dummy_log = {0};
        bb_err_t e = bb_event_subscribe(topic, record_handler, &dummy_log, &subs[i]);
        if (e != BB_OK) {
            break;
        }
    }

    // Now try ring_subscribe_with_replay; should fail when trying to subscribe
    handler_log_t final_log = {0};
    bb_event_sub_t final_sub = NULL;
    err = bb_event_ring_subscribe_with_replay(ring, record_handler, &final_log, &final_sub);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    // Clean up
    for (int j = 0; j < i; j++) {
        bb_event_unsubscribe(subs[j]);
    }
    bb_event_ring_detach(ring);
}

static int seen_id;
static size_t seen_size;
static void ring_probe_capture(bb_event_topic_t topic, int32_t id, const void *data, size_t size, void *user) {
    (void)topic; (void)data; (void)user;
    seen_id = id;
    seen_size = size;
}
static void dummy_handler(bb_event_topic_t topic, int32_t id, const void *data, size_t size, void *user) {
    (void)topic; (void)id; (void)data; (void)size; (void)user;
}

/* Covers line 70 false branch in ring_capture: size > 0 && data is FALSE
   when data is NULL even though size > 0.
   bb_event_post(topic, id, NULL, 8) — passes NULL data with positive size.
   Ring captures the metadata (id, size) but skips memcpy. */
void test_bb_event_ring_capture_null_data_with_size(void) {
    setup_sync_mode();
    bb_event_init(&small_cfg);
    bb_event_topic_t topic;
    bb_event_topic_register("ring.nulldata", &topic);
    bb_event_ring_t ring;
    bb_event_ring_attach(topic, 4, 32, &ring);

    bb_err_t err = bb_event_post(topic, 99, NULL, 8);
    TEST_ASSERT_EQUAL(BB_OK, err);
    bb_event_pump(0);

    /* Replay to a probe and assert id was captured. */
    seen_id = -1;
    seen_size = 0;
    struct {
        bb_event_sub_t sub;
    } cap;
    bb_event_ring_subscribe_with_replay(ring, ring_probe_capture, &cap, &cap.sub);
    /* ring_probe_capture sets seen_id and seen_size — defined as a static helper. */
    TEST_ASSERT_EQUAL(99, seen_id);
    TEST_ASSERT_EQUAL(8, seen_size);

    bb_event_unsubscribe(cap.sub);
    bb_event_ring_detach(ring);
}

/* Covers line 133: bb_event_subscribe failure inside bb_event_ring_attach.
   Force subscribe to fail by exhausting the subscriber pool first. */
void test_bb_event_ring_attach_subscribe_failure_frees_all(void) {
    setup_sync_mode();
    bb_event_init(&small_cfg);
    bb_event_topic_t fill_topic;
    bb_event_topic_register("ringfill.topic", &fill_topic);

    /* Fill subscriber pool. BB_EVENT_MAX_SUBSCRIBERS = CONFIG_BB_EVENT_MAX_TOPICS * 8 = 512. */
    static bb_event_sub_t filler[512];
    int filled = 0;
    while (filled < 512) {
        bb_err_t e = bb_event_subscribe(fill_topic, dummy_handler, NULL, &filler[filled]);
        if (e != BB_OK) break;
        filled++;
    }

    /* Now ring_attach's internal subscribe will fail. */
    bb_event_topic_t ring_topic;
    bb_event_topic_register("ring.fail.attach", &ring_topic);
    bb_event_ring_t r;
    bb_err_t err = bb_event_ring_attach(ring_topic, 4, 16, &r);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    /* Cleanup. */
    for (int i = 0; i < filled; i++) bb_event_unsubscribe(filler[i]);
}
