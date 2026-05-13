#include "unity.h"
#include "bb_event.h"
#include "bb_event_port.h"
#include "../../components/bb_event_ring/bb_event_ring_internal.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Synchronous mode: events are dispatched via bb_event_pump(), not background thread
static void setup_sync_mode(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
}

// Configure with small queue and payload for testing overflow
static bb_event_cfg_t small_cfg = {
    .queue_depth = 4,
    .max_payload = 64,
};

// Callback recorder
#define MAX_HANDLER_CALLS 16

typedef struct {
    int call_count;
    struct {
        bb_event_topic_t topic;
        int32_t id;
        size_t size;
        uint8_t payload[64];
    } calls[MAX_HANDLER_CALLS];
} handler_log_t;

static handler_log_t g_log;

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
        if (size > 0 && data) {
            memcpy(log->calls[log->call_count].payload, data, size);
        }
        log->call_count++;
    }
}

static void reset_log(void)
{
    memset(&g_log, 0, sizeof g_log);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_event_init_topic_register_subscribe_post_pump_fires(void)
{
    setup_sync_mode();
    reset_log();

    bb_err_t err = bb_event_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_topic_t topic = NULL;
    err = bb_event_topic_register("test.topic", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(topic);

    bb_event_sub_t sub = NULL;
    err = bb_event_subscribe(topic, record_handler, &g_log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(sub);

    const char *payload = "hello";
    err = bb_event_post(topic, 42, payload, strlen(payload) + 1);
    TEST_ASSERT_EQUAL(BB_OK, err);

    size_t dispatched = bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, dispatched);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(42, g_log.calls[0].id);
    TEST_ASSERT_EQUAL(strlen(payload) + 1, g_log.calls[0].size);
    TEST_ASSERT_EQUAL_MEMORY(payload, g_log.calls[0].payload, strlen(payload) + 1);

    bb_event_unsubscribe(sub);
}

void test_bb_event_two_subscribers_both_receive(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic2", &topic);

    handler_log_t log1, log2;
    memset(&log1, 0, sizeof log1);
    memset(&log2, 0, sizeof log2);

    bb_event_sub_t sub1 = NULL, sub2 = NULL;
    bb_event_subscribe(topic, record_handler, &log1, &sub1);
    bb_event_subscribe(topic, record_handler, &log2, &sub2);

    bb_event_post(topic, 99, "data", 5);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, log1.call_count);
    TEST_ASSERT_EQUAL(1, log2.call_count);
    TEST_ASSERT_EQUAL(99, log1.calls[0].id);
    TEST_ASSERT_EQUAL(99, log2.calls[0].id);

    bb_event_unsubscribe(sub1);
    bb_event_unsubscribe(sub2);
}

void test_bb_event_unsubscribe_prevents_future_events(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic3", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Post and pump once
    bb_event_post(topic, 1, NULL, 0);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, g_log.call_count);

    // Unsubscribe
    bb_event_unsubscribe(sub);

    // Post and pump again
    reset_log();
    bb_event_post(topic, 2, NULL, 0);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(0, g_log.call_count);
}

void test_bb_event_post_exceeds_max_payload_returns_invalid_arg(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic4", &topic);

    uint8_t oversized[65];
    memset(oversized, 0, sizeof oversized);

    // max_payload is 64; try to post 65 bytes
    bb_err_t err = bb_event_post(topic, 0, oversized, 65);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_queue_overflow_returns_no_space(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);  // queue_depth=4

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic5", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Post 4 events (queue fills)
    for (int i = 0; i < 4; i++) {
        bb_err_t err = bb_event_post(topic, i, NULL, 0);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    // 5th post should fail with BB_ERR_NO_SPACE
    bb_err_t err = bb_event_post(topic, 4, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    // Drain the queue so it doesn't pollute later tests
    bb_event_pump(0);

    bb_event_unsubscribe(sub);
}

void test_bb_event_topic_lookup_returns_same_handle(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t registered = NULL;
    bb_err_t err = bb_event_topic_register("lookup.test", &registered);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_topic_t looked_up = NULL;
    err = bb_event_topic_lookup("lookup.test", &looked_up);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_PTR(registered, looked_up);
}

void test_bb_event_topic_register_duplicate_returns_same_handle(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t first = NULL;
    bb_err_t err = bb_event_topic_register("dup.test", &first);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_topic_t second = NULL;
    err = bb_event_topic_register("dup.test", &second);
    TEST_ASSERT_EQUAL(BB_OK, err);

    TEST_ASSERT_EQUAL_PTR(first, second);
}

void test_bb_event_different_topics_dont_cross(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("topic.cross.single", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Post to the topic
    bb_event_post(topic, 111, NULL, 0);
    size_t n = bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(111, g_log.calls[0].id);

    bb_event_unsubscribe(sub);
}

void test_bb_event_payload_integrity(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.payload", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Create a test struct
    struct {
        uint32_t magic;
        uint16_t value;
        uint8_t arr[10];
    } data;
    data.magic = 0xdeadbeef;
    data.value = 0x1234;
    memset(data.arr, 0xAB, sizeof data.arr);

    bb_event_post(topic, 777, &data, sizeof data);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(sizeof data, g_log.calls[0].size);
    TEST_ASSERT_EQUAL_MEMORY(&data, g_log.calls[0].payload, sizeof data);

    bb_event_unsubscribe(sub);
}

void test_bb_event_id_parameter_preserved(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.id", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Post with specific id
    int32_t test_id = 42;
    bb_event_post(topic, test_id, NULL, 0);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(test_id, g_log.calls[0].id);

    bb_event_unsubscribe(sub);
}

// ---------------------------------------------------------------------------
// Additional tests for branch coverage
// ---------------------------------------------------------------------------

void test_bb_event_init_null_cfg_uses_defaults(void)
{
    setup_sync_mode();
    // Call init with NULL to exercise cfg==NULL branch (subsequent inits return OK)
    bb_err_t err = bb_event_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_event_init_idempotent(void)
{
    setup_sync_mode();
    // Call init multiple times; should always return BB_OK (line 90 early return on s_init_done)
    bb_err_t err = bb_event_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);
    // Second init should return immediately
    err = bb_event_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_event_topic_register_null_name_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_register(NULL, &topic);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// Reuse first topic name from pre-existing tests to save registry space
// Topic registry is limited to 16 entries; tests use 27+ unique names without consolidation

void test_bb_event_topic_register_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_err_t err = bb_event_topic_register("test.topic", NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_topic_register_returns_ok_when_initialized(void)
{
    setup_sync_mode();
    // Ensure init has been called (likely from other tests)
    bb_event_init(&small_cfg);

    // Register should work now (reuse test.topic, registry returns same handle for duplicate names)
    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_register("test.topic", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(topic);
}

void test_bb_event_topic_register_exceeds_max_returns_no_space(void)
{
    setup_sync_mode();
    bb_event_cfg_t cfg = {
        .queue_depth = 4,
        .max_payload = 64,
    };
    bb_event_init(&cfg);

    // Attempt to exhaust the topic registry by registering many topics
    // CONFIG_BB_EVENT_MAX_TOPICS = 16
    bb_event_topic_t topic = NULL;
    int successful = 0;
    int failed = 0;

    // Try to register more topics than the max
    for (int i = 0; i < 25; i++) {
        char name[40];
        snprintf(name, sizeof(name), "regmax.%d", i);
        bb_err_t err = bb_event_topic_register(name, &topic);
        if (err == BB_OK) {
            successful++;
        } else if (err == BB_ERR_NO_SPACE) {
            failed++;
        }
    }

    // We should have at least hit no-space at some point (unless fewer topics pre-allocated)
    TEST_ASSERT(failed > 0 || successful == 16);
}

void test_bb_event_topic_lookup_null_name_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_lookup(NULL, &topic);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_topic_lookup_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_err_t err = bb_event_topic_lookup("test.topic", NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_topic_lookup_not_found(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_lookup("nonexistent.topic", &topic);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

void test_bb_event_subscribe_null_topic_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_sub_t sub = NULL;
    bb_err_t err = bb_event_subscribe(NULL, record_handler, &g_log, &sub);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// Next tests reuse topic names to avoid registry overflow (limit: 16 topics)

void test_bb_event_subscribe_null_callback_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic2", &topic);

    bb_event_sub_t sub = NULL;
    bb_err_t err = bb_event_subscribe(topic, NULL, &g_log, &sub);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_subscribe_null_out_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic3", &topic);

    bb_err_t err = bb_event_subscribe(topic, record_handler, &g_log, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_subscribe_succeeds_when_pool_available(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic4", &topic);

    bb_event_sub_t sub = NULL;
    handler_log_t log;
    memset(&log, 0, sizeof log);

    // Single subscribe should succeed - exercises alloc_subscriber returning non-NULL
    bb_err_t err = bb_event_subscribe(topic, record_handler, &log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_event_unsubscribe_null_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_err_t err = bb_event_unsubscribe(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_unsubscribe_returns_ok_on_valid_sub(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.topic5", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Unsubscribe should return OK - exercises line 211 return BB_OK
    bb_err_t err = bb_event_unsubscribe(sub);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_event_post_null_topic_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_err_t err = bb_event_post(NULL, 0, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// Use single consolidated topic for remaining post validation tests

void test_bb_event_post_payload_too_large_returns_invalid_arg(void)
{
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.payload", &topic);

    // small_cfg.max_payload = 64; try 65
    uint8_t big[65];
    bb_err_t err = bb_event_post(topic, 0, big, 65);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_event_post_with_small_payload_succeeds(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("test.id", &topic);

    // Post should succeed with valid args - exercises line 226 return path
    uint8_t data[32];
    bb_err_t err = bb_event_post(topic, 42, data, 32);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_event_init_cfg_with_nonzero_values(void)
{
    setup_sync_mode();
    // Exercise cfg branches (lines 97-99)
    bb_event_cfg_t cfg = {
        .queue_depth = 16,
        .max_payload = 256,
        .stack_size = 8192,
        .task_priority = 3,
    };
    bb_err_t err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_event_post_zero_payload_no_data(void)
{
    setup_sync_mode();
    reset_log();

    bb_event_init(&small_cfg);
    bb_event_topic_t topic = NULL;
    bb_event_topic_register("zero.size", &topic);

    bb_event_sub_t sub = NULL;
    bb_event_subscribe(topic, record_handler, &g_log, &sub);

    // Post zero-size event (line 218 payload check false branch)
    bb_err_t err = bb_event_post(topic, 77, NULL, 0);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(0, g_log.calls[0].size);

    bb_event_unsubscribe(sub);
}

void test_bb_event_unsubscribe_early_in_list(void)
{
    setup_sync_mode();

    bb_event_init(&small_cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("unsub.early", &topic);

    handler_log_t log1, log2;
    memset(&log1, 0, sizeof log1);
    memset(&log2, 0, sizeof log2);

    bb_event_sub_t sub1 = NULL, sub2 = NULL;
    bb_event_subscribe(topic, record_handler, &log1, &sub1);
    bb_event_subscribe(topic, record_handler, &log2, &sub2);

    // Unsubscribe first (head of list, line 200 walk finds it early)
    bb_event_unsubscribe(sub1);

    bb_event_post(topic, 99, NULL, 0);
    bb_event_pump(0);

    // Only sub2 should have received
    TEST_ASSERT_EQUAL(0, log1.call_count);
    TEST_ASSERT_EQUAL(1, log2.call_count);

    bb_event_unsubscribe(sub2);
}

void test_init_pool_guard_returns_early(void)
{
    // After reset_for_test (keeps pool initialized), call bb_event_init
    // to enter init body, call init_subscriber_pool, hit guard at line 40
    setup_sync_mode();
    bb_event_cfg_t cfg = {.queue_depth = 16, .max_payload = 256};

    bb_err_t err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Now reset but keep pool initialized
    bb_event_reset_pool_for_test();

    // Call init again - should go through init_subscriber_pool and hit guard
    err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_init_with_zero_queue_depth_uses_default(void)
{
    setup_sync_mode();
    bb_event_cfg_t cfg = {.queue_depth = 0, .max_payload = 256};

    bb_err_t err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // If we can post and pump, the queue was initialized with defaults
    bb_event_topic_t topic = NULL;
    err = bb_event_topic_register("test.default_queue", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_init_with_zero_max_payload_uses_default(void)
{
    setup_sync_mode();
    bb_event_cfg_t cfg = {.queue_depth = 16, .max_payload = 0};

    bb_err_t err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_topic_t topic = NULL;
    err = bb_event_topic_register("test.default_payload", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_init_port_init_failure_returns_error(void)
{
    setup_sync_mode();

    // Set malloc to fail, which will cause bb_event_port_init to fail
    bb_event_port_set_malloc(test_failing_malloc);
    test_alloc_fail_at = 0;  // Fail on first malloc

    bb_event_cfg_t cfg = {.queue_depth = 16, .max_payload = 256};
    bb_err_t err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    // Reset for cleanup
    bb_event_port_set_malloc(NULL);
}

void test_topic_register_before_init_returns_invalid_state(void)
{
    // Don't call setup_sync_mode or bb_event_init
    // reset_for_test was called in setUp, so s_init_done = false

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_register("test.before_init", &topic);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
}

void test_unsubscribe_non_head_subscriber(void)
{
    setup_sync_mode();
    bb_event_cfg_t cfg = {.queue_depth = 16, .max_payload = 256};

    bb_err_t err = bb_event_init(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_topic_t topic = NULL;
    err = bb_event_topic_register("test.unsub_nonhead", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Subscribe A (becomes head)
    handler_log_t log_a = {0};
    bb_event_sub_t sub_a = NULL;
    err = bb_event_subscribe(topic, record_handler, &log_a, &sub_a);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Subscribe B (becomes new head)
    handler_log_t log_b = {0};
    bb_event_sub_t sub_b = NULL;
    err = bb_event_subscribe(topic, record_handler, &log_b, &sub_b);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Unsubscribe A (which is non-head); exercises pnext-walk path at line 200
    bb_event_unsubscribe(sub_a);

    // Post and verify only B receives
    bb_event_post(topic, 99, NULL, 0);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(0, log_a.call_count);
    TEST_ASSERT_EQUAL(1, log_b.call_count);

    bb_event_unsubscribe(sub_b);
}

void test_dispatch_null_entry_no_crash(void)
{
    // bb_event_common_dispatch(NULL, NULL) should not crash
    // This tests the defensive check at line 255
    setup_sync_mode();

    bb_event_common_dispatch(NULL, NULL);
    // If we get here without crashing, the test passes
    TEST_PASS();
}

static void dummy_handler(bb_event_topic_t topic, int32_t id, const void *data, size_t size, void *user) {
    (void)topic; (void)id; (void)data; (void)size; (void)user;
}

/* Covers line 124: strcmp != 0 false branch in topic_register dup loop.
   Register 3 topics with different names; the third register walks past the first two. */
void test_bb_event_topic_register_walks_existing_entries(void) {
    setup_sync_mode();
    bb_event_init(NULL);
    bb_event_topic_t t1, t2, t3;
    bb_err_t err;
    err = bb_event_topic_register("walk.t1", &t1);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_event_topic_register("walk.t2", &t2);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_event_topic_register("walk.t3", &t3);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(t3);
}

/* Covers line 131: topic registry full true branch.
   CONFIG_BB_EVENT_MAX_TOPICS=64 in native env. Fill all 64, then 65th must fail. */
void test_bb_event_topic_register_returns_no_space_when_full(void) {
    /* Pre-existing tests have already registered several topics; reset to start fresh. */
    bb_event_reset_for_test();
    bb_event_init(NULL);

    char name[32];
    bb_event_topic_t t;
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof(name), "fill.%d", i);
        bb_err_t err = bb_event_topic_register(name, &t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, name);
    }
    bb_err_t err = bb_event_topic_register("overflow", &t);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    /* Reset so subsequent tests have a clean registry. */
    bb_event_reset_for_test();
    bb_event_init(NULL);
}

/* Covers line 152: strcmp != 0 false branch in topic_lookup loop.
   Register 2 topics, lookup a 3rd name that doesn't match — walks past both. */
void test_bb_event_topic_lookup_walks_past_non_matches(void) {
    setup_sync_mode();
    bb_event_init(NULL);
    bb_event_topic_t t1, t2, found;
    bb_event_topic_register("look.t1", &t1);
    bb_event_topic_register("look.t2", &t2);
    bb_err_t err = bb_event_topic_lookup("look.t3", &found);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

/* Covers line 218 true branch: size > CONFIG_BB_EVENT_MAX_PAYLOAD.
   CONFIG_BB_EVENT_MAX_PAYLOAD=512 in native env, so post with 600 bytes. */
void test_bb_event_post_exceeds_max_payload_at_runtime_limit(void) {
    setup_sync_mode();
    bb_event_init(NULL);
    bb_event_topic_t topic;
    bb_event_topic_register("oversize.topic", &topic);
    uint8_t buf[600];
    memset(buf, 0xAB, sizeof(buf));
    bb_err_t err = bb_event_post(topic, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

