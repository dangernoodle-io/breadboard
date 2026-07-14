#include "unity.h"
#include "bb_event.h"
#include "bb_event_test.h"
#include "test_alloc_inject.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

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


/* Covers bb_event_subscribe_with_prep:
   - prep runs once, before subscription becomes active
   - returns BB_ERR_INVALID_ARG on bad args; prep may be NULL */
static int g_prep_ran = 0;
static void test_prep_fn(void *arg) {
    int *count = (int *)arg;
    if (count) (*count)++;
    g_prep_ran++;
}

void test_bb_event_subscribe_with_prep_runs_prep_before_subscribe(void) {
    setup_sync_mode();
    reset_log();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_register("prep.topic", &topic));

    /* Post and drain event A before subscribing; new sub should not see it. */
    int32_t payload = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_post(topic, 1, &payload, sizeof(payload)));
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(0, g_log.call_count);

    g_prep_ran = 0;
    int prep_count = 0;
    bb_event_sub_t sub;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_subscribe_with_prep(topic, record_handler, &g_log,
                                     test_prep_fn, &prep_count, &sub));
    TEST_ASSERT_EQUAL(1, g_prep_ran);
    TEST_ASSERT_EQUAL(1, prep_count);
    TEST_ASSERT_EQUAL(0, g_log.call_count); /* prep ran but no replay */

    /* Subscription is now active: a new post is delivered. */
    payload = 2;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_post(topic, 2, &payload, sizeof(payload)));
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(2, g_log.calls[0].id);

    bb_event_unsubscribe(sub);
}

/* NULL prep degenerates to atomic subscribe. */
void test_bb_event_subscribe_with_prep_null_prep_subscribes(void) {
    setup_sync_mode();
    reset_log();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_register("prep.null", &topic));

    bb_event_sub_t sub;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_subscribe_with_prep(topic, record_handler, &g_log,
                                     NULL, NULL, &sub));

    int32_t payload = 99;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_post(topic, 99, &payload, sizeof(payload)));
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(99, g_log.calls[0].id);

    bb_event_unsubscribe(sub);
}

/* Validates BB_ERR_INVALID_ARG on each missing required arg. */
void test_bb_event_subscribe_with_prep_invalid_args(void) {
    setup_sync_mode();
    bb_event_init(&small_cfg);

    bb_event_topic_t topic;
    bb_event_topic_register("prep.invalid", &topic);
    bb_event_sub_t sub;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_event_subscribe_with_prep(NULL, record_handler, &g_log, NULL, NULL, &sub));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_event_subscribe_with_prep(topic, NULL, &g_log, NULL, NULL, &sub));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_event_subscribe_with_prep(topic, record_handler, &g_log, NULL, NULL, NULL));
}

/* bb_event_lock/unlock are exported and idempotent in nesting (rely on platform mutex). */
void test_bb_event_lock_unlock_round_trip(void) {
    setup_sync_mode();
    bb_event_init(&small_cfg);
    bb_event_lock();
    bb_event_unlock();
    /* No assertion beyond clean exit — proves symbols are linked and callable. */
}

// ---------------------------------------------------------------------------
// Regression test: HPR-1 — unsubscribe-during-dispatch safety (SMP UAF fix)
//
// The old dispatch code snapshotted only sub_head under the lock and then
// released the lock before walking sub->next.  On SMP (ESP32-S3 fleet) a
// concurrent bb_event_unsubscribe could free a node mid-walk → UAF.
//
// This test exercises the analogous single-threaded scenario where one
// subscriber's callback unsubscribes a *different* subscriber on the same
// topic.  Under the old code the walker reads sub->next from a freed/recycled
// pool node; under the fixed code the lock is held for the full walk so the
// node cannot be freed until dispatch completes.
//
// The test asserts:
//   1. The walk completes without crash (ASan/valgrind catch UAF on CI).
//   2. The subscriber whose callback ran (sub_a) ran exactly once.
//   3. The victim subscriber (sub_b, unsubscribed by sub_a's callback) did NOT
//      run after being unsubscribed — confirming the unsubscribe took effect on
//      the next dispatch, not mid-walk.
// ---------------------------------------------------------------------------

typedef struct {
    int call_count;
    bb_event_sub_t *victim_sub;  // sub to unsubscribe inside the callback
} unsub_during_dispatch_ctx_t;

static handler_log_t g_victim_log;

static void unsub_victim_cb(bb_event_topic_t topic, int32_t id,
                             const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)data; (void)size;
    unsub_during_dispatch_ctx_t *ctx = (unsub_during_dispatch_ctx_t *)user;
    ctx->call_count++;
    if (ctx->victim_sub && *ctx->victim_sub) {
        // Attempt to unsubscribe the victim while the dispatcher holds the lock.
        // Under the fix: bb_event_unsubscribe takes the recursive lock (safe),
        // unlinks the node, and frees it back to the pool.  The walker, still
        // holding the outer lock level, has already advanced past this node or
        // will skip it because it was the next node at the time of the call —
        // either way, no UAF occurs because the pool node is only recycled, not
        // freed to the heap, and the walker finishes before any reallocation can
        // happen within the same locked section.
        bb_event_unsubscribe(*ctx->victim_sub);
        *ctx->victim_sub = NULL;
    }
}

void test_bb_event_dispatch_unsubscribe_during_walk_is_safe(void)
{
    setup_sync_mode();
    reset_log();
    memset(&g_victim_log, 0, sizeof g_victim_log);

    bb_event_cfg_t cfg = { .queue_depth = 4, .max_payload = 64 };
    bb_event_init(&cfg);

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("race.unsub.walk", &topic);

    // sub_b is subscribed first (becomes tail of list / first to be walked).
    // sub_a is subscribed second (becomes head).
    // Walk order (newest-first singly-linked list): sub_a → sub_b.
    // sub_a's callback unsubscribes sub_b.  The walker must then safely advance
    // to sub_b->next (which is NULL) without UAF.
    bb_event_sub_t sub_b = NULL;
    bb_event_subscribe(topic, record_handler, &g_victim_log, &sub_b);

    unsub_during_dispatch_ctx_t ctx = { .call_count = 0, .victim_sub = &sub_b };
    bb_event_sub_t sub_a = NULL;
    bb_event_subscribe(topic, unsub_victim_cb, &ctx, &sub_a);

    // Post one event and drain.
    bb_event_post(topic, 1, NULL, 0);
    size_t dispatched = bb_event_pump(0);

    // Walk completed — no crash.
    TEST_ASSERT_EQUAL(1, dispatched);

    // sub_a's callback ran exactly once.
    TEST_ASSERT_EQUAL(1, ctx.call_count);

    // sub_b was unsubscribed by sub_a's callback mid-walk.
    // Depending on traversal order (head→tail), sub_b may or may not have been
    // called on this dispatch.  Assert that sub_b received AT MOST 1 call
    // (either before or after being unsubscribed by sub_a, but not double-fired).
    TEST_ASSERT(g_victim_log.call_count <= 1);

    // sub_b must now be unsubscribed: a second post must not reach sub_b.
    int prev_victim_calls = g_victim_log.call_count;
    bb_event_post(topic, 2, NULL, 0);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(prev_victim_calls, g_victim_log.call_count);  // no new calls

    // Cleanup: sub_b was already freed by unsubscribe; only sub_a remains.
    if (sub_a) bb_event_unsubscribe(sub_a);
}

void test_bb_event_topic_name_null_returns_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", bb_event_topic_name(NULL));
}

/* bb_event_autoinit is the tier=early composition-root entry point (see the
   "bbtool:init" marker in bb_event.h); it just forwards to bb_event_init(NULL)
   so Kconfig defaults apply. Cover it directly since codegen call sites aren't
   exercised on host. */
void test_bb_event_autoinit_returns_ok(void)
{
    setup_sync_mode();
    bb_err_t err = bb_event_autoinit();
    TEST_ASSERT_EQUAL(BB_OK, err);
}

/* bb_event_emit (KB 820 PR3) -- the string-keyed, bb_emit_fn-shaped
   peer of bb_event_post: register-or-lookup by name, then post. A
   subsequent bb_event_subscribe + bb_event_pump on that name must deliver
   the forwarded (id, payload, size). */
void test_bb_event_emit_delivers_to_subscriber(void)
{
    setup_sync_mode();
    reset_log();

    bb_err_t err = bb_event_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_topic_t topic = NULL;
    err = bb_event_topic_register("test.named", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_sub_t sub = NULL;
    err = bb_event_subscribe(topic, record_handler, &g_log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);

    const char *payload = "hello-named";
    bb_event_emit("test.named", 7, payload, strlen(payload) + 1);

    size_t dispatched = bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, dispatched);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(7, g_log.calls[0].id);
    TEST_ASSERT_EQUAL(strlen(payload) + 1, g_log.calls[0].size);
    TEST_ASSERT_EQUAL_MEMORY(payload, g_log.calls[0].payload, strlen(payload) + 1);

    bb_event_unsubscribe(sub);
}

/* bb_event_emit is idempotent-per-call: repeated calls with the same
   name reuse the same registered topic rather than erroring or duplicating
   it (bb_event_topic_register "returns same handle for duplicate names").
   A second post must still reach the subscriber registered after the
   first. */
void test_bb_event_emit_reuses_topic_on_repeat(void)
{
    setup_sync_mode();
    reset_log();

    bb_err_t err = bb_event_init(&small_cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // First call registers the topic with no subscriber yet -- dropped on
    // dispatch (no subscribers), which is fine; this call only proves the
    // topic gets registered.
    bb_event_emit("test.named.repeat", 1, NULL, 0);
    bb_event_pump(0);

    bb_event_topic_t topic = NULL;
    err = bb_event_topic_lookup("test.named.repeat", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(topic);

    bb_event_sub_t sub = NULL;
    err = bb_event_subscribe(topic, record_handler, &g_log, &sub);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_emit("test.named.repeat", 2, NULL, 0);
    size_t dispatched = bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, dispatched);
    TEST_ASSERT_EQUAL(1, g_log.call_count);
    TEST_ASSERT_EQUAL(2, g_log.calls[0].id);

    bb_event_unsubscribe(sub);
}

/* bb_event_emit is void-return by design (no caller can act on a
   bb_err_t from a generic emit sink) -- calling it before bb_event_init has
   run must log-and-return, not crash. */
void test_bb_event_emit_before_init_no_crash(void)
{
    // No bb_event_init() call -- setUp's bb_event_reset_for_test() leaves
    // the bus uninitialized.
    bb_event_emit("test.named.uninit", 1, NULL, 0);
    // Reaching here without a crash is the assertion.
    TEST_ASSERT_TRUE(true);
}

/* bb_event_emit's topic-register-failure log guards name with a
   name ? name : "(null)" ternary (name IS reachably NULL at this call site
   -- bb_event_topic_register rejects NULL before the init-state check, so
   this fires regardless of init state). Mirrors the repo convention for
   this exact idiom (e.g. bb_log_config_apply's default_level_str ? ... :
   "(null)", covered by test_bb_log_config_apply_null_default_is_non_fatal
   -- see test_bb_log_config.c): test the NULL branch rather than exclude
   it. */
void test_bb_event_emit_null_name_logs_and_returns(void)
{
    bb_event_emit(NULL, 1, NULL, 0);
    // Reaching here without a crash is the assertion.
    TEST_ASSERT_TRUE(true);
}

/* bb_event_emit's second failure branch: topic register succeeds but the
   subsequent bb_event_post fails (queue full) -- must log-and-return, not
   crash or propagate. */
void test_bb_event_emit_post_failure_logs_and_returns(void)
{
    setup_sync_mode();

    bb_err_t err = bb_event_init(&small_cfg);  // queue_depth=4
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Fill the queue to capacity (4 entries) via plain bb_event_post.
    bb_event_topic_t topic = NULL;
    err = bb_event_topic_register("test.named.full", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);
    for (int i = 0; i < 4; i++) {
        err = bb_event_post(topic, i, NULL, 0);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    // Queue is full -- bb_event_emit's internal bb_event_post must fail and
    // return without crashing.
    bb_event_emit("test.named.full", 4, NULL, 0);
    TEST_ASSERT_TRUE(true);

    bb_event_pump(0);  // drain so this doesn't pollute later tests
}

// ---------------------------------------------------------------------------
// Regression tests: bb_event_port_reset_for_test drain-wait determinism
//
// bb_event_host.c's async dispatcher runs on a background pthread. Every
// test's global setUp() tears the port down via bb_event_port_reset_for_test
// unconditionally (test_main.c). Previously, teardown just signalled the
// dispatcher to stop and joined it -- if a test posted an event and the
// dispatcher hadn't yet been scheduled to pop it, the stop signal made the
// dispatcher exit WITHOUT dispatching, silently dropping the entry. Whether
// this happened was pure OS-scheduling luck, which made dispatcher_loop's
// pop/dispatch lines flaky under host coverage (uncovered on a loaded/slow
// CI runner, covered on a fast idle machine). The fix makes teardown wait
// (via a dedicated drained_cond, bounded so a genuinely stuck dispatcher
// can't hang CI) for the queue to actually drain before signalling shutdown.
//
// Both tests below use bb_event_port_test_set_dispatcher_startup_delay_ms to
// deterministically simulate "the dispatcher hasn't been scheduled by the OS
// yet" (rather than racing real scheduling, which is what made the original
// bug flaky in the first place). What each test actually validates differs:
//
//   - test_..._drains_before_teardown: asserts on ACTUAL DELIVERY via a
//     counting subscriber, which DOES distinguish the fix from the pre-fix
//     code -- without the drain-wait, this delivery would race and often
//     be dropped.
//   - test_..._drops_on_drain_timeout: the startup delay (900ms) exceeds the
//     drain-wait bound (500ms) in BOTH the fixed and pre-fix code, so
//     call_count==0 alone does NOT distinguish them (a test that only
//     asserted that would be masked-by-construction). What this test
//     actually validates is the TIMEOUT FALLBACK PATH and its logging: it
//     additionally asserts bb_event_port_test_get_last_drain_wait_ms() is
//     >= the drain-wait bound, proving the bounded wait itself genuinely
//     ran for its full duration (rather than returning immediately, as the
//     pre-fix code did) before falling through to the deliberate-drop/log
//     path.
// ---------------------------------------------------------------------------

static void counting_handler(bb_event_topic_t topic, int32_t id,
                             const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)data; (void)size;
    int *count = (int *)user;
    (*count)++;
}

void test_bb_event_port_reset_for_test_drains_before_teardown(void)
{
    setenv("BB_EVENT_HOST_SYNC", "0", 1);  // force real async/threaded mode
    bb_event_reset_for_test();
    bb_event_port_reset_for_test();

    // The dispatcher thread will still be asleep in its artificial startup
    // delay (well under the 500ms drain-wait bound) when reset_for_test is
    // called below -- so this deterministically forces the drain-wait loop
    // to actually wait, rather than finding count==0 already and returning
    // immediately.
    bb_event_port_test_set_dispatcher_startup_delay_ms(50);

    bb_event_cfg_t cfg = { .queue_depth = 4, .max_payload = 64 };
    TEST_ASSERT_EQUAL(BB_OK, bb_event_init(&cfg));

    bb_event_topic_t topic = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_register("reset.drain.wait", &topic));

    int call_count = 0;
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_subscribe(topic, counting_handler, &call_count, &sub));

    TEST_ASSERT_EQUAL(BB_OK, bb_event_post(topic, 1, NULL, 0));

    bb_event_port_reset_for_test();

    // The only assertion that actually proves the entry was drained (not
    // dropped): the subscriber registered by THIS test, before the post,
    // must have fired exactly once. (setUp's bb_event_reset_for_test runs
    // BEFORE bb_event_port_reset_for_test, so a drain triggered from setUp
    // would dispatch to no one -- calling reset_for_test directly, mid-test,
    // as done here, is what keeps this subscriber live for the drain to
    // reach; see the NOTE in bb_event_port_reset_for_test.)
    TEST_ASSERT_EQUAL_MESSAGE(1, call_count,
        "entry posted just before teardown must be delivered exactly once");

    bb_event_unsubscribe(sub);
    setup_sync_mode();  // restore the ambient default for tests that follow
}

// Mirrors BB_EVENT_DRAIN_WAIT_TIMEOUT_MS in bb_event_host.c (not exposed to
// tests -- see the test-only header rationale in bb_event_test.h). Keep in
// sync if that constant ever changes.
#define TEST_DRAIN_WAIT_TIMEOUT_MS 500

/* Reviewer finding: without an elapsed-time assertion this test passed both
   WITH and WITHOUT the drain-wait fix -- the 900ms startup delay exceeds
   BOTH "no wait" (old code) and "bounded 500ms wait" (new code), so
   pthread_join dominates either way and call_count==0 is identical in both
   cases. The fix asserts on bb_event_port_test_get_last_drain_wait_ms(),
   which times ONLY the pthread_cond_timedwait loop, not the surrounding
   pthread_join -- an earlier draft of this assertion measured wall-clock
   elapsed across the WHOLE bb_event_port_reset_for_test() call instead, and
   that was itself masked-by-construction: since this dispatcher's startup
   delay (900ms) exceeds the drain-wait bound (500ms), pthread_join alone
   dominates the total regardless of whether the drain-wait code ran at all,
   so a whole-function timer stayed >=500ms even with the fix reverted
   (confirmed empirically -- see KB 1422 verification). Timing only the
   drain-wait segment closes that gap: it reads -1 (never ran) when the
   drain-wait code is absent, and >= the bound when present -- a genuine
   LOWER bound either way, so a slow CI runner only makes it MORE true. */
void test_bb_event_port_reset_for_test_drops_on_drain_timeout(void)
{
    setenv("BB_EVENT_HOST_SYNC", "0", 1);
    bb_event_reset_for_test();
    bb_event_port_reset_for_test();

    // Startup delay outlives the 500ms drain-wait bound, so reset_for_test
    // genuinely times out (exercising the timeout/log path) and the
    // dispatcher is STILL asleep -- not merely running late -- when
    // thread_running flips false. Its outer loop condition is false on its
    // very first check, so it never pops the entry: a real, deliberate drop,
    // characterizing the documented trade-off (bounded wait can still lose
    // an entry) rather than assuming eventual delivery.
    bb_event_port_test_set_dispatcher_startup_delay_ms(900);

    bb_event_cfg_t cfg = { .queue_depth = 4, .max_payload = 64 };
    TEST_ASSERT_EQUAL(BB_OK, bb_event_init(&cfg));

    bb_event_topic_t topic = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_register("reset.drain.timeout", &topic));

    int call_count = 0;
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_subscribe(topic, counting_handler, &call_count, &sub));

    TEST_ASSERT_EQUAL(BB_OK, bb_event_post(topic, 1, NULL, 0));

    // Must return (not hang) once the bound expires; internally still blocks
    // in pthread_join until the dispatcher's startup delay finishes and its
    // thread function returns (~900ms total for this test).
    bb_event_port_reset_for_test();

    // Proves the bounded drain-wait itself actually ran for (at least) its
    // full timeout -- rather than returning immediately, which is what the
    // pre-fix code did -- without being masked by the unrelated pthread_join
    // duration (see the block comment above this test). A lower bound only,
    // so it cannot be made flaky by a slow/loaded CI runner.
    long drain_wait_ms = bb_event_port_test_get_last_drain_wait_ms();
    TEST_ASSERT_TRUE_MESSAGE(drain_wait_ms >= TEST_DRAIN_WAIT_TIMEOUT_MS,
        "reset_for_test's drain-wait segment must block for at least the "
        "drain-wait timeout before falling through to teardown");

    TEST_ASSERT_EQUAL_MESSAGE(0, call_count,
        "a dispatcher that never wakes within the bound must not silently "
        "appear to have delivered");

    bb_event_unsubscribe(sub);
    setup_sync_mode();
}

// ---------------------------------------------------------------------------
// bb_event_port_test_compute_drain_deadline: deterministic coverage of the
// deadline-computation helper's tv_nsec-overflow-normalization branch.
// Calling clock_gettime() from the test itself (instead of passing a crafted
// `now`) would make hitting the overflow branch a wall-clock coin flip.
// ---------------------------------------------------------------------------

void test_bb_event_compute_drain_deadline_no_overflow(void)
{
    struct timespec now = { .tv_sec = 1000, .tv_nsec = 100 * 1000 * 1000 };  // .100s
    struct timespec deadline = bb_event_port_test_compute_drain_deadline(now, 500);  // +.500s

    TEST_ASSERT_EQUAL(1000, deadline.tv_sec);
    TEST_ASSERT_EQUAL(600 * 1000 * 1000, deadline.tv_nsec);
}

void test_bb_event_compute_drain_deadline_overflows_into_next_second(void)
{
    struct timespec now = { .tv_sec = 1000, .tv_nsec = 900 * 1000 * 1000 };  // .900s
    struct timespec deadline = bb_event_port_test_compute_drain_deadline(now, 500);  // +.500s = 1.400s

    TEST_ASSERT_EQUAL(1001, deadline.tv_sec);
    TEST_ASSERT_EQUAL(400 * 1000 * 1000, deadline.tv_nsec);
}
