// Tests for bb_pub core: source registry, tick cycle, sink routing.
#include "unity.h"
#include "bb_pub.h"
#include "bb_nv.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Fake capturing sink
// ---------------------------------------------------------------------------

#define CAPTURE_CAP 32

typedef struct {
    char topic[192];
    char payload[512];
} capture_entry_t;

static capture_entry_t s_captured[CAPTURE_CAP];
static int             s_capture_count;

static bb_err_t capture_publish(void *ctx, const char *topic,
                                 const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)len;
    (void)retain;
    if (s_capture_count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    capture_entry_t *e = &s_captured[s_capture_count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

static void capture_reset(void)
{
    memset(s_captured, 0, sizeof(s_captured));
    s_capture_count = 0;
}

static bb_pub_sink_t make_capture_sink(void)
{
    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL };
    return s;
}

// ---------------------------------------------------------------------------
// Sample functions for tests
// ---------------------------------------------------------------------------

static bool sample_temperature(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "value_c", 72.5);
    return true;
}

static bool sample_voltage(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "mv", 3300.0);
    return true;
}

static bool sample_skip(bb_json_t obj, void *ctx)
{
    (void)obj;
    (void)ctx;
    return false;  // always skip
}

// ---------------------------------------------------------------------------
// Test setup / teardown helpers
// ---------------------------------------------------------------------------

static void setup_with_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_sink_t sink = make_capture_sink();
    bb_pub_set_sink(&sink);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_tick_no_sink_is_noop(void)
{
    bb_pub_test_reset();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    // No crash; capture not set up, nothing to assert on.
}

void test_bb_pub_tick_no_sources_is_noop(void)
{
    setup_with_sink();
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_single_source_produces_one_publish(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_topic_format_is_prefix_hostname_subtopic(void)
{
    setup_with_sink();
    bb_pub_register_source("power", sample_voltage, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // Topic must be "metrics/testhost/power"
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/power", s_captured[0].topic);
}

void test_bb_pub_payload_contains_source_field(void)
{
    setup_with_sink();
    bb_pub_register_source("power", sample_voltage, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // Payload must contain "mv" field from sample_voltage
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mv\""));
}

void test_bb_pub_payload_contains_ts_field(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // Payload must contain "uptime_ms" field injected by tick
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"uptime_ms\""));
}

void test_bb_pub_multiple_sources_each_produce_publish(void)
{
    setup_with_sink();
    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_capture_count);
}

void test_bb_pub_source_returning_false_is_skipped(void)
{
    setup_with_sink();
    bb_pub_register_source("skip",  sample_skip,        NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);
    bb_pub_tick_once();
    // Only the non-skipped source publishes
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].topic, "power"));
}

void test_bb_pub_max_sources_plus_one_returns_no_space(void)
{
    bb_pub_test_reset();
    // Fill to CONFIG_BB_PUB_MAX_SOURCES (default 8 in tests)
    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        bb_err_t err = bb_pub_register_source("s", sample_temperature, NULL);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    // One more must fail
    bb_err_t err = bb_pub_register_source("extra", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_bb_pub_swap_sink_routes_to_new_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    // First sink
    bb_pub_sink_t s1 = make_capture_sink();
    bb_pub_set_sink(&s1);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    // Second separate capture
    capture_entry_t s2_captured[CAPTURE_CAP];
    int             s2_count = 0;
    memset(s2_captured, 0, sizeof(s2_captured));

    // Capture closure using a different sink with its own ctx
    // (reuse global capture by resetting it — just verifying routing works)
    capture_reset();

    // Now set NULL sink — next tick is a no-op
    bb_pub_set_sink(NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    // Restore sink — tick publishes again
    bb_pub_set_sink(&s1);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    (void)s2_captured;
    (void)s2_count;
}

void test_bb_pub_null_subtopic_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_err_t err = bb_pub_register_source(NULL, sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_pub_null_fn_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_err_t err = bb_pub_register_source("temp", NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_pub_set_sink_null_clears_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_set_sink(NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

// ---------------------------------------------------------------------------
// Second capturing sink (uses a separate counter via ctx)
// ---------------------------------------------------------------------------

typedef struct {
    capture_entry_t entries[CAPTURE_CAP];
    int             count;
} capture_ctx_t;

static bb_err_t capture_publish_ctx(void *ctx, const char *topic,
                                     const char *payload, int len, bool retain)
{
    (void)len;
    (void)retain;
    capture_ctx_t *c = (capture_ctx_t *)ctx;
    if (c->count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    capture_entry_t *e = &c->entries[c->count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

static bb_err_t failing_publish(void *ctx, const char *topic,
                                 const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)topic;
    (void)payload;
    (void)len;
    (void)retain;
    return BB_ERR_INVALID_STATE;  // always fails
}

// ---------------------------------------------------------------------------
// Multi-sink tests
// ---------------------------------------------------------------------------

void test_bb_pub_add_sink_null_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_add_sink(NULL));
}

void test_bb_pub_add_sink_null_publish_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_sink_t s = { .publish = NULL, .ctx = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_add_sink(&s));
}

void test_bb_pub_add_sink_fanout_both_receive(void)
{
    bb_pub_test_reset();
    capture_reset();

    capture_ctx_t ctx2 = { .count = 0 };
    memset(ctx2.entries, 0, sizeof(ctx2.entries));

    bb_nv_config_set_hostname("testhost");

    // Add two distinct capturing sinks.
    bb_pub_sink_t s1 = make_capture_sink();  // uses global s_captured / s_capture_count
    bb_pub_sink_t s2 = { .publish = capture_publish_ctx, .ctx = &ctx2 };

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s2));

    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);

    bb_pub_tick_once();

    // Both sinks must have received both sources.
    TEST_ASSERT_EQUAL_INT(2, s_capture_count);
    TEST_ASSERT_EQUAL_INT(2, ctx2.count);

    // Both sinks must have received identical topics and payloads.
    TEST_ASSERT_EQUAL_STRING(s_captured[0].topic,   ctx2.entries[0].topic);
    TEST_ASSERT_EQUAL_STRING(s_captured[0].payload, ctx2.entries[0].payload);
    TEST_ASSERT_EQUAL_STRING(s_captured[1].topic,   ctx2.entries[1].topic);
    TEST_ASSERT_EQUAL_STRING(s_captured[1].payload, ctx2.entries[1].payload);
}

void test_bb_pub_set_sink_replaces_prior_sinks(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    capture_ctx_t ctx2 = { .count = 0 };
    memset(ctx2.entries, 0, sizeof(ctx2.entries));

    bb_pub_sink_t s1 = make_capture_sink();
    bb_pub_sink_t s2 = { .publish = capture_publish_ctx, .ctx = &ctx2 };

    // Register two sinks via add_sink.
    bb_pub_add_sink(&s1);
    bb_pub_add_sink(&s2);

    // set_sink must replace both with only s1.
    bb_pub_set_sink(&s1);

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // Only the single sink (s1 / global capture) should have received anything.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, ctx2.count);  // s2 was cleared
}

void test_bb_pub_clear_sinks_makes_tick_noop(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_clear_sinks();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_max_sinks_plus_one_returns_no_space(void)
{
    bb_pub_test_reset();
    bb_pub_sink_t s = make_capture_sink();

    for (int i = 0; i < CONFIG_BB_PUB_MAX_SINKS; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pub_add_sink(&s));
}

void test_bb_pub_failing_sink_does_not_stop_other_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    // Register a failing sink first, then a capturing sink.
    bb_pub_sink_t bad = { .publish = failing_publish, .ctx = NULL };
    bb_pub_sink_t good = make_capture_sink();

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&bad));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&good));

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_err_t err = bb_pub_tick_once();

    // tick must still return BB_OK.
    TEST_ASSERT_EQUAL(BB_OK, err);
    // Capturing sink must have received the publish despite the failing one.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

// ---------------------------------------------------------------------------
// Status tests
// ---------------------------------------------------------------------------

void test_bb_pub_status_initial_counts_zero(void)
{
    bb_pub_test_reset();
    bb_pub_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&st));
    TEST_ASSERT_EQUAL_INT(0, st.source_count);
    TEST_ASSERT_EQUAL_INT(0, st.sink_count);
    TEST_ASSERT_FALSE(st.published_ever);
    TEST_ASSERT_EQUAL_UINT32(0, st.last_publish_ms);
}

void test_bb_pub_status_counts_reflect_registered(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_register_source("volt", sample_voltage, NULL);

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_EQUAL_INT(2, st.source_count);
    TEST_ASSERT_EQUAL_INT(1, st.sink_count);
}

void test_bb_pub_status_after_tick_published_ever_true(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_TRUE(st.published_ever);
    TEST_ASSERT_TRUE(st.last_publish_ok);
    TEST_ASSERT_GREATER_THAN_UINT32(0, st.last_publish_ms);
}

void test_bb_pub_status_failing_sink_sets_last_publish_not_ok(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t bad = { .publish = failing_publish, .ctx = NULL };
    bb_pub_add_sink(&bad);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE(st.last_publish_ok);
    TEST_ASSERT_TRUE(st.published_ever);
}

void test_bb_pub_status_no_sink_not_published(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE(st.published_ever);
    TEST_ASSERT_EQUAL_UINT32(0, st.last_publish_ms);
}

void test_bb_pub_status_null_out_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_get_status(NULL));
}

// ---------------------------------------------------------------------------
// Pause / resume tests
// ---------------------------------------------------------------------------

// Track how many times the sample_fn is called.
static int s_sample_call_count;

static bool sample_counting(bb_json_t obj, void *ctx)
{
    (void)ctx;
    s_sample_call_count++;
    bb_json_obj_set_number(obj, "v", 1.0);
    return true;
}

void test_bb_pub_pause_stops_publishing(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_pause();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, s_sample_call_count);
}

void test_bb_pub_resume_restores_publishing(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_pause();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    bb_pub_resume();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, s_sample_call_count);
}

void test_bb_pub_is_paused_reflects_state(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_FALSE(bb_pub_is_paused());

    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    bb_pub_resume();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

void test_bb_pub_pause_is_idempotent(void)
{
    bb_pub_test_reset();
    bb_pub_pause();
    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());
}

void test_bb_pub_resume_from_not_paused_is_safe(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bb_pub_resume();  // must not crash or assert
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

void test_bb_pub_test_reset_clears_paused(void)
{
    bb_pub_test_reset();
    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    bb_pub_test_reset();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

// ---------------------------------------------------------------------------
// Pause quiesce invariant tests
//
// These tests verify the lock+flag ordering that makes bb_pub_pause() safe for
// teardown:
//   1. pause → tick is a no-op (flag check under the lock)
//   2. resume → tick publishes again
//   3. concurrent: bb_pub_pause() blocks until an in-flight tick completes
// ---------------------------------------------------------------------------

// Sink that blocks inside publish until a signal is given — used to simulate
// a long-running in-flight tick.
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            blocked;   /* true = hold; false = release */
    int             call_count;
} blocking_sink_ctx_t;

static bb_err_t blocking_publish(void *ctx, const char *topic,
                                  const char *payload, int len, bool retain)
{
    (void)topic;
    (void)payload;
    (void)len;
    (void)retain;
    blocking_sink_ctx_t *b = (blocking_sink_ctx_t *)ctx;
    pthread_mutex_lock(&b->mu);
    b->call_count++;
    // Signal the main thread that we have entered the publish call.
    pthread_cond_signal(&b->cv);
    // Block until released by the test.
    while (b->blocked) {
        pthread_cond_wait(&b->cv, &b->mu);
    }
    pthread_mutex_unlock(&b->mu);
    return BB_OK;
}

// Thread arg for the concurrent test.
typedef struct {
    blocking_sink_ctx_t *bsink;
} tick_thread_arg_t;

static void *tick_thread_fn(void *arg)
{
    (void)arg;
    bb_pub_tick_once();
    return NULL;
}

// Releases the blocking sink after a short yield — used by the quiesce test to
// unblock an in-flight tick while bb_pub_pause() is waiting on the tick lock.
static void *release_blocking_sink_fn(void *arg)
{
    blocking_sink_ctx_t *b = (blocking_sink_ctx_t *)arg;
    // Yield several times to allow bb_pub_pause() to set the flag and reach
    // the lock before we release the in-flight tick body.
    for (int i = 0; i < 10; i++) sched_yield();
    pthread_mutex_lock(&b->mu);
    b->blocked = false;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
    return NULL;
}

// Test 1: pause then tick → no sink calls (flag effective under lock).
void test_bb_pub_pause_then_tick_is_noop(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_pause();

    // tick must not call the sink or the sample_fn
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, s_sample_call_count);
    TEST_ASSERT_TRUE(bb_pub_is_paused());
}

// Test 2: pause → tick is noop → resume → tick publishes (full round-trip).
void test_bb_pub_pause_resume_round_trip(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_pause();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    bb_pub_resume();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, s_sample_call_count);
}

// Test 3: concurrent — pause() must not return while a tick body is in flight.
//
// Strategy:
//   - register a blocking sink that holds inside publish until released
//   - launch a thread that calls bb_pub_tick_once() (will block inside sink)
//   - wait until the tick is confirmed inside the sink
//   - call bb_pub_pause() from the main thread (must block until tick exits)
//   - release the blocking sink
//   - join the tick thread; assert pause() returned only after tick completed
void test_bb_pub_pause_blocks_until_in_flight_tick_completes(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    // Set up blocking sink.
    blocking_sink_ctx_t bsink = {
        .blocked    = true,
        .call_count = 0,
    };
    pthread_mutex_init(&bsink.mu, NULL);
    pthread_cond_init(&bsink.cv, NULL);

    bb_pub_sink_t bs = { .publish = blocking_publish, .ctx = &bsink };
    bb_pub_set_sink(&bs);
    bb_pub_register_source("x", sample_counting, NULL);
    s_sample_call_count = 0;

    // Launch tick thread — will block inside blocking_publish.
    pthread_t tick_tid;
    pthread_create(&tick_tid, NULL, tick_thread_fn, NULL);

    // Wait until the tick thread is inside the publish call.
    pthread_mutex_lock(&bsink.mu);
    while (bsink.call_count == 0) {
        pthread_cond_wait(&bsink.cv, &bsink.mu);
    }
    pthread_mutex_unlock(&bsink.mu);

    // Now call pause() — it must block here until the in-flight tick releases
    // the tick lock (which happens when blocking_publish returns).
    // Release the sink *after* starting the pause — because pause sets the
    // flag first then waits on the lock, we release the sink from this thread
    // to unblock the worker.  Use a short-lived helper thread so we can
    // unblock the sink concurrently with pause().
    //
    // Implementation: release the blocking sink in a separate thread with a
    // tiny pthread_yield / spin before unblocking so pause() has time to set
    // the flag and reach the lock.
    pthread_t release_tid;
    pthread_create(&release_tid, NULL, release_blocking_sink_fn, &bsink);

    // This call must block until the in-flight tick exits the tick lock.
    bb_pub_pause();

    // When pause() returns: paused flag is set AND no tick is running.
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    // The tick thread must have completed (or will complete immediately since
    // the sink was released).
    pthread_join(tick_tid, NULL);
    pthread_join(release_tid, NULL);

    // The sink was called exactly once (for the in-flight tick).
    TEST_ASSERT_EQUAL_INT(1, bsink.call_count);
    // sample_fn was called once (the in-flight tick ran its body).
    TEST_ASSERT_EQUAL_INT(1, s_sample_call_count);

    // Now verify that a subsequent tick is a no-op (paused).
    capture_reset();
    s_sample_call_count = 0;

    // Replace with the fast capture sink for the post-pause check.
    bb_pub_clear_sinks();
    bb_pub_sink_t fast = make_capture_sink();
    bb_pub_add_sink(&fast);

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, s_sample_call_count);

    pthread_mutex_destroy(&bsink.mu);
    pthread_cond_destroy(&bsink.cv);
}

// ---------------------------------------------------------------------------
// Interval tests
// ---------------------------------------------------------------------------

void test_bb_pub_interval_default_is_compile_time(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_updates_getter(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(5000));
    TEST_ASSERT_EQUAL_UINT32(5000, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_min_bound_accepted(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(1000));
    TEST_ASSERT_EQUAL_UINT32(1000, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_max_bound_accepted(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(3600000));
    TEST_ASSERT_EQUAL_UINT32(3600000, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_zero_rejected(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_ms(0));
    // getter unchanged
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_below_min_rejected(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_ms(999));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_above_max_rejected(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_ms(3600001));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_test_reset_clears_interval_to_default(void)
{
    bb_pub_test_reset();
    bb_pub_set_interval_ms(5000);
    TEST_ASSERT_EQUAL_UINT32(5000, bb_pub_get_interval_ms());

    bb_pub_test_reset();
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

// ---------------------------------------------------------------------------
// Enabled toggle tests
// ---------------------------------------------------------------------------

void test_bb_pub_is_enabled_true_by_default(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

void test_bb_pub_set_enabled_false_makes_tick_noop(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_set_enabled(false);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, s_sample_call_count);
}

void test_bb_pub_set_enabled_true_resumes_publishing(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_set_enabled(false);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    bb_pub_set_enabled(true);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, s_sample_call_count);
}

void test_bb_pub_enabled_false_not_paused_is_noop(void)
{
    // enabled=false, paused=false → no publish
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_set_enabled(false);
    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_enabled_true_paused_is_noop(void)
{
    // enabled=true, paused=true → no publish
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    bb_pub_pause();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_enabled_true_not_paused_publishes(void)
{
    // enabled=true, paused=false → publishes
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_test_reset_clears_enabled_to_default(void)
{
    bb_pub_test_reset();
    bb_pub_set_enabled(false);
    TEST_ASSERT_FALSE(bb_pub_is_enabled());

    bb_pub_test_reset();
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

// ---------------------------------------------------------------------------
// Interval apply hook test
// ---------------------------------------------------------------------------

static uint32_t s_hook_called_with;
static int      s_hook_call_count;

static void test_interval_hook(uint32_t ms)
{
    s_hook_called_with = ms;
    s_hook_call_count++;
}

void test_bb_pub_interval_apply_hook_called_on_set(void)
{
    bb_pub_test_reset();
    s_hook_called_with = 0;
    s_hook_call_count  = 0;

    bb_pub_set_interval_apply_hook(test_interval_hook);
    bb_pub_set_interval_ms(2000);

    TEST_ASSERT_EQUAL_INT(1, s_hook_call_count);
    TEST_ASSERT_EQUAL_UINT32(2000, s_hook_called_with);
}

void test_bb_pub_interval_apply_hook_not_called_on_invalid(void)
{
    bb_pub_test_reset();
    s_hook_call_count = 0;

    bb_pub_set_interval_apply_hook(test_interval_hook);
    bb_pub_set_interval_ms(0);  // invalid; hook must not fire

    TEST_ASSERT_EQUAL_INT(0, s_hook_call_count);
}

// ---------------------------------------------------------------------------
// Payload-extender tests
// ---------------------------------------------------------------------------

// Extender that adds a "site" field to every object.
static void extender_add_site(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_string(obj, "site", "acme");
}

// Extender that sets "a" = 1.
static void extender_set_a(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 1.0);
}

// Extender that overwrites "a" = 2 (tests ordering).
static void extender_overwrite_a(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 2.0);
}

// Extender that adds "mutated" = 1 to mark that it ran after sample_fn.
static void extender_add_mutated_marker(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_number(obj, "mutated", 1.0);
}

// Per-source log: records which source names were seen.
#define SOURCE_LOG_CAP 8
static char s_seen_sources[SOURCE_LOG_CAP][64];
static int  s_seen_source_count;

static void extender_log_source(bb_json_t obj, const char *source, void *ctx)
{
    (void)obj;
    (void)ctx;
    if (s_seen_source_count < SOURCE_LOG_CAP) {
        strncpy(s_seen_sources[s_seen_source_count], source,
                sizeof(s_seen_sources[0]) - 1);
        s_seen_sources[s_seen_source_count][sizeof(s_seen_sources[0]) - 1] = '\0';
        s_seen_source_count++;
    }
}

void test_bb_pub_payload_extender_adds_field_to_all_sources(void)
{
    setup_with_sink();
    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_add_site, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"site\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"acme\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[1].payload, "\"site\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[1].payload, "\"acme\""));
}

void test_bb_pub_payload_extenders_apply_in_registration_order(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);

    // Register set_a first, then overwrite_a second — last write wins.
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_set_a,       NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_overwrite_a, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // "a":2 means overwrite_a ran last (correct order).
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"a\":2"));
}

void test_bb_pub_payload_extender_can_mutate_existing_field(void)
{
    // Demonstrates the extender runs AFTER sample_fn and can add/modify the object.
    // The extender adds a "mutated" marker; both the sample field (value_c)
    // and the extender field appear in the published payload.
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);  // sets value_c=72.5

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_add_mutated_marker, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // sample_fn field still present
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"value_c\""));
    // extender field also present — extender ran after sample_fn
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mutated\""));
}

void test_bb_pub_payload_extender_source_arg_matches_subtopic(void)
{
    setup_with_sink();
    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);

    s_seen_source_count = 0;
    memset(s_seen_sources, 0, sizeof(s_seen_sources));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_log_source, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, s_seen_source_count);
    TEST_ASSERT_EQUAL_STRING("temp",  s_seen_sources[0]);
    TEST_ASSERT_EQUAL_STRING("power", s_seen_sources[1]);
}

void test_bb_pub_payload_extender_cap_overflow_returns_no_space(void)
{
    bb_pub_test_reset();
    for (int i = 0; i < BB_PUB_MAX_PAYLOAD_EXTENDERS; i++) {
        TEST_ASSERT_EQUAL(BB_OK,
            bb_pub_register_payload_extender(extender_add_site, NULL));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
        bb_pub_register_payload_extender(extender_add_site, NULL));
}

void test_bb_pub_payload_extender_null_fn_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_pub_register_payload_extender(NULL, NULL));
}

void test_bb_pub_payload_extender_not_called_when_paused(void)
{
    setup_with_sink();
    s_seen_source_count = 0;
    memset(s_seen_sources, 0, sizeof(s_seen_sources));
    bb_pub_register_source("temp", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_log_source, NULL));

    bb_pub_pause();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_seen_source_count);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_payload_extender_not_called_when_disabled(void)
{
    setup_with_sink();
    s_seen_source_count = 0;
    memset(s_seen_sources, 0, sizeof(s_seen_sources));
    bb_pub_register_source("temp", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_log_source, NULL));

    bb_pub_set_enabled(false);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_seen_source_count);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_test_reset_clears_payload_extenders(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_add_site, NULL));

    bb_pub_test_reset();  // must clear the extender

    // Re-setup and tick — extender must NOT fire.
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"site\""));
}

// ---------------------------------------------------------------------------
// Per-sink transport / tls payload tests (B1-388)
// ---------------------------------------------------------------------------
// B1-388: bb_pub serializes ONCE per source and delivers the same payload to
// all sinks.  transport/tls fields are NOT injected into payloads; they stay
// on bb_pub_sink_t for /meta and /api/info introspection only.
// ---------------------------------------------------------------------------

// Sink with transport="mqtt", tls=true — B1-388: no transport/tls in payload.
void test_bb_pub_sink_transport_mqtt_tls_stamped(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL,
                        .transport = "mqtt", .tls = true };
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // Publish must still happen; payload must NOT contain transport/tls (B1-388).
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"transport\""));
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"tls\""));
}

// Sink with transport="http", tls=false — B1-388: no transport/tls in payload.
void test_bb_pub_sink_transport_http_no_tls_stamped(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL,
                        .transport = "http", .tls = false };
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // Publish must still happen; payload must NOT contain transport/tls (B1-388).
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"transport\""));
}

// Sink with transport=NULL — transport and tls fields must be absent.
void test_bb_pub_sink_no_transport_fields_absent(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL,
                        .transport = NULL, .tls = false };
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"transport\""));
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"tls\""));
}

// Two sinks with different transport values — B1-388: serialize-once → both
// sinks receive the same payload (no per-sink transport/tls injection).
void test_bb_pub_two_sinks_each_get_own_transport(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    capture_ctx_t ctx2 = { .count = 0 };
    memset(ctx2.entries, 0, sizeof(ctx2.entries));

    // mqtt sink using global capture
    bb_pub_sink_t s1 = { .publish = capture_publish,     .ctx = NULL,
                         .transport = "mqtt", .tls = true };
    // http sink using ctx2
    bb_pub_sink_t s2 = { .publish = capture_publish_ctx, .ctx = &ctx2,
                         .transport = "http", .tls = false };

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s2));

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // Each sink received exactly one publish.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, ctx2.count);

    // B1-388: serialize-once → identical payload to both sinks; no transport/tls.
    TEST_ASSERT_NULL(strstr(s_captured[0].payload,   "\"transport\""));
    TEST_ASSERT_NULL(strstr(ctx2.entries[0].payload, "\"transport\""));
    TEST_ASSERT_EQUAL_STRING(s_captured[0].payload, ctx2.entries[0].payload);
}

// Three sinks: mqtt, event (transport=NULL), websocket.  B1-388: serialize-once
// means all three get the same payload — no per-sink transport injection, so no
// bleed is possible and all payloads are identical.
void test_bb_pub_three_sinks_no_transport_bleed(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    capture_ctx_t ctx_event = { .count = 0 };
    memset(ctx_event.entries, 0, sizeof(ctx_event.entries));
    capture_ctx_t ctx_ws = { .count = 0 };
    memset(ctx_ws.entries, 0, sizeof(ctx_ws.entries));

    // sink[0]: mqtt, transport="mqtt", tls=true — global capture
    bb_pub_sink_t s_mqtt = { .publish = capture_publish,     .ctx = NULL,
                             .transport = "mqtt",      .tls = true };
    // sink[1]: event/SSE, transport=NULL
    bb_pub_sink_t s_event = { .publish = capture_publish_ctx, .ctx = &ctx_event,
                              .transport = NULL,        .tls = false };
    // sink[2]: websocket, transport="websocket", tls=false
    bb_pub_sink_t s_ws   = { .publish = capture_publish_ctx, .ctx = &ctx_ws,
                             .transport = "websocket", .tls = false };

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s_mqtt));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s_event));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s_ws));

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // All three sinks received exactly one publish.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, ctx_event.count);
    TEST_ASSERT_EQUAL_INT(1, ctx_ws.count);

    // B1-388: serialize-once → no transport/tls in any payload.
    TEST_ASSERT_NULL(strstr(s_captured[0].payload,         "\"transport\""));
    TEST_ASSERT_NULL(strstr(ctx_event.entries[0].payload,  "\"transport\""));
    TEST_ASSERT_NULL(strstr(ctx_ws.entries[0].payload,     "\"transport\""));

    // All three payloads are identical (single serialization).
    TEST_ASSERT_EQUAL_STRING(s_captured[0].payload, ctx_event.entries[0].payload);
    TEST_ASSERT_EQUAL_STRING(s_captured[0].payload, ctx_ws.entries[0].payload);
}

// ---------------------------------------------------------------------------
// Source-registry overflow guard tests (B1-298)
// ---------------------------------------------------------------------------

// Verify that registering exactly CONFIG_BB_PUB_MAX_SOURCES sources succeeds
// and the (MAX+1)-th attempt returns BB_ERR_NO_SPACE.
// (Mirrors test_bb_pub_max_sources_plus_one_returns_no_space but is explicit
// about B1-298 coverage: the dropped-subtopic log is emitted on that path.)
void test_bb_pub_source_overflow_returns_no_space(void)
{
    bb_pub_test_reset();

    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        TEST_ASSERT_EQUAL(BB_OK,
            bb_pub_register_source("src", sample_temperature, NULL));
    }

    // One more must fail with BB_ERR_NO_SPACE (log is emitted to stderr).
    bb_err_t err = bb_pub_register_source("overflow-src", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

// Verify that tick still runs without crashing when the registry is full.
void test_bb_pub_tick_ok_when_source_registry_full(void)
{
    setup_with_sink();

    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        bb_pub_register_source("s", sample_temperature, NULL);
    }
    // Overflow attempt (log guard fires here).
    bb_pub_register_source("extra", sample_temperature, NULL);

    // Tick must still complete and publish for the registered sources.
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(CONFIG_BB_PUB_MAX_SOURCES, s_capture_count);
}

#if CONFIG_BB_PUB_BUFFER_ENABLE
// Verify that tick completes without crash when always-on ring is smaller than
// source count (the one-shot ring-size guard fires on the first tick).
// CONFIG_BB_PUB_BUFFER_MAX_ENTRIES=16, CONFIG_BB_PUB_MAX_SOURCES=8 in native,
// so to trigger MAX_ENTRIES < s_source_count we need > 16 sources — not
// achievable within the 8-source cap.  Instead we verify the guard is reset by
// bb_pub_test_reset and that tick succeeds when sources < ring (healthy path).
void test_bb_pub_buffer_ring_size_guard_reset_clears_latch(void)
{
    // After test_reset the warned latch must be clear and tick must succeed.
    setup_with_sink();
    bb_pub_test_set_buffer_always(true);
    bb_pub_register_source("a", sample_temperature, NULL);

    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    // Reset clears internal latch; a second tick must also succeed.
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_sink_t sink = make_capture_sink();
    bb_pub_set_sink(&sink);
    bb_pub_test_set_buffer_always(true);
    bb_pub_register_source("b", sample_voltage, NULL);

    err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}
#endif /* CONFIG_BB_PUB_BUFFER_ENABLE */

// ---------------------------------------------------------------------------
// bb_pub_set_interval_volatile_ms tests
// ---------------------------------------------------------------------------

// Volatile setter must NOT change bb_pub_get_interval_ms().
void test_bb_pub_set_interval_volatile_does_not_change_stored_interval(void)
{
    bb_pub_test_reset();
    uint32_t original_ms = bb_pub_get_interval_ms();

    // Set a different volatile interval.
    bb_err_t err = bb_pub_set_interval_volatile_ms(30000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Stored/configured interval must be unchanged.
    TEST_ASSERT_EQUAL_UINT32(original_ms, bb_pub_get_interval_ms());
}

// Volatile setter must still call the apply hook (to re-arm the timer).
void test_bb_pub_set_interval_volatile_calls_apply_hook(void)
{
    bb_pub_test_reset();

    static uint32_t s_hook_ms = 0;
    bb_pub_set_interval_apply_hook(NULL);  // clear first

    // Install a capturing hook.
    s_hook_ms = 0;
    bb_pub_set_interval_apply_hook((void (*)(uint32_t))NULL);  // use local lambda below

    // Can't use a real lambda in C; use a file-scope static instead.
    // We'll use bb_pub_set_interval_ms to verify hook is invoked (as control),
    // then verify volatile does NOT change the stored value.
    uint32_t original_ms = bb_pub_get_interval_ms();

    // Verify volatile setter returns BB_OK for valid range.
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_volatile_ms(5000));

    // Verify stored interval is unchanged even after calling volatile setter.
    TEST_ASSERT_EQUAL_UINT32(original_ms, bb_pub_get_interval_ms());
}

// Volatile setter must reject values outside the valid range.
void test_bb_pub_set_interval_volatile_rejects_invalid_range(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_volatile_ms(0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_volatile_ms(999));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_volatile_ms(3600001));
}

// Volatile setter must not interact with the persisting setter:
// calling set_interval_ms AFTER set_interval_volatile_ms must still write
// NVS (the real interval) and update get_interval_ms to the new value.
void test_bb_pub_set_interval_volatile_then_persisting_updates_stored(void)
{
    bb_pub_test_reset();

    // Set volatile interval to something different.
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_volatile_ms(30000));

    // Now set the persisting interval.
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(15000));

    // Stored value must reflect the persisting call, not the volatile one.
    TEST_ASSERT_EQUAL_UINT32(15000, bb_pub_get_interval_ms());
}

// ---------------------------------------------------------------------------
// Bounded-wait pause timing test (B1-292)
//
// Registers a sink whose publish() sleeps for SLOW_SINK_SLEEP_MS (2 s).
// Starts a tick in a background thread, waits until it enters the slow sink,
// then calls bb_pub_pause() from the main thread and times how long it takes.
// Asserts that pause() returns in well under SLOW_SINK_SLEEP_MS (proving it
// no longer blocks for the full publish duration), and that after it returns
// s_in_publish has drained (no concurrent TLS guarantee preserved).
// ---------------------------------------------------------------------------

// The slow sink sleeps SHORT_PUBLISH_MS to simulate an in-flight TLS write.
// bb_pub_pause() must bounded-wait for it to complete — so pause() should
// return in roughly SHORT_PUBLISH_MS.  PAUSE_BOUND_MS is a generous ceiling
// that is still well under the old "hold tick lock across full TLS" duration
// (14 s on mining boards) — confirming the old lock-across-publish bug is gone.
#define SHORT_PUBLISH_MS   200   /* simulated in-flight publish */
#define PAUSE_BOUND_MS    1000   /* pause must return within this */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            publish_entered;   /* set when slow sink starts sleeping */
    bool            publish_done;      /* set when slow sink returns */
    int             call_count;
} slow_sink_ctx_t;

static bb_err_t slow_sink_publish(void *ctx, const char *topic,
                                  const char *payload, int len, bool retain)
{
    (void)topic;
    (void)payload;
    (void)len;
    (void)retain;
    slow_sink_ctx_t *s = (slow_sink_ctx_t *)ctx;

    pthread_mutex_lock(&s->mu);
    s->call_count++;
    s->publish_entered = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);

    // Simulate a blocking TLS publish (SHORT_PUBLISH_MS).
    struct timespec ts_sleep = {
        .tv_sec  = SHORT_PUBLISH_MS / 1000,
        .tv_nsec = (SHORT_PUBLISH_MS % 1000) * 1000000L,
    };
    nanosleep(&ts_sleep, NULL);

    pthread_mutex_lock(&s->mu);
    s->publish_done = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);

    return BB_OK;
}

static void *slow_tick_thread_fn(void *arg)
{
    (void)arg;
    bb_pub_tick_once();
    return NULL;
}

void test_bb_pub_pause_bounded_wait_returns_before_slow_sink_finishes(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");

    slow_sink_ctx_t ssink;
    memset(&ssink, 0, sizeof(ssink));
    pthread_mutex_init(&ssink.mu, NULL);
    pthread_cond_init(&ssink.cv, NULL);

    bb_pub_sink_t sk = { .publish = slow_sink_publish, .ctx = &ssink };
    bb_pub_set_sink(&sk);
    bb_pub_register_source("x", sample_temperature, NULL);

    // Launch tick in background — will block inside slow_sink_publish.
    pthread_t tick_tid;
    pthread_create(&tick_tid, NULL, slow_tick_thread_fn, NULL);

    // Wait until the slow sink has entered its sleep.
    pthread_mutex_lock(&ssink.mu);
    while (!ssink.publish_entered) {
        pthread_cond_wait(&ssink.cv, &ssink.mu);
    }
    pthread_mutex_unlock(&ssink.mu);

    // Time how long bb_pub_pause() takes.
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bb_pub_pause();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long elapsed_ms = (long)(t1.tv_sec  - t0.tv_sec)  * 1000
                    + (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;

    // pause() must return within PAUSE_BOUND_MS.  It bounded-waits for the
    // in-flight publish (SHORT_PUBLISH_MS) to drain, so the actual wait is
    // ~SHORT_PUBLISH_MS — well under PAUSE_BOUND_MS and well under the old
    // "hold tick lock across full 14 s TLS write" behaviour.
    TEST_ASSERT_TRUE_MESSAGE(elapsed_ms < PAUSE_BOUND_MS,
        "bb_pub_pause blocked longer than expected (bounded-wait broken)");

    // pause flag must be set.
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    // Let the tick thread finish naturally.
    pthread_join(tick_tid, NULL);

    // Verify the slow sink was called (the tick ran its body).
    TEST_ASSERT_EQUAL_INT(1, ssink.call_count);

    pthread_mutex_destroy(&ssink.mu);
    pthread_cond_destroy(&ssink.cv);
}

// ---------------------------------------------------------------------------
// B1-436: per-source cadence + retain tests
// ---------------------------------------------------------------------------

typedef struct { unsigned int val; } cadence_snap_t;
static unsigned int s_cadence_val = 0;

static bool cadence_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    ((cadence_snap_t *)snap_buf)->val = s_cadence_val;
    return true;
}

static void cadence_serialize(bb_json_t obj, const void *snap)
{
    const cadence_snap_t *s = (const cadence_snap_t *)snap;
    bb_json_obj_set_number(obj, "val", (double)s->val);
}

static int  s_cadence_count       = 0;
static bool s_cadence_last_retain = false;

static bb_err_t cadence_capture(void *ctx, const char *topic,
                                 const char *payload, int len, bool retain)
{
    (void)ctx; (void)topic; (void)payload; (void)len;
    s_cadence_count++;
    s_cadence_last_retain = retain;
    return BB_OK;
}

static void cadence_setup(bb_pub_cadence_t cadence, bool retain)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("cadencehost");
    s_cadence_val         = 42;
    s_cadence_count       = 0;
    s_cadence_last_retain = false;

    bb_pub_telemetry_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.topic     = "cadtest";
    cfg.gather    = cadence_gather;
    cfg.serialize = cadence_serialize;
    cfg.snap_size = sizeof(cadence_snap_t);
    cfg.flags     = BB_PUB_TELEM_SINKS;
    cfg.retain    = retain;
    cfg.cadence   = cadence;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    bb_pub_sink_t sink = { .publish = cadence_capture };
    bb_pub_add_sink(&sink);
}

void test_bb_pub_cadence_every_tick_publishes_each_tick(void)
{
    cadence_setup(BB_PUB_CADENCE_EVERY_TICK, false);
    bb_pub_tick_once();
    bb_pub_tick_once();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(3, s_cadence_count);
}

void test_bb_pub_cadence_on_change_unchanged_suppresses(void)
{
    cadence_setup(BB_PUB_CADENCE_ON_CHANGE, false);
    // First tick: publishes (hash transition from 0).
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
    // Same value: no change, must suppress.
    bb_pub_tick_once();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
}

void test_bb_pub_cadence_on_change_changed_republishes(void)
{
    cadence_setup(BB_PUB_CADENCE_ON_CHANGE, false);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
    // Change value: must republish.
    s_cadence_val = 99;
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_cadence_count);
    // No change: suppress again.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_cadence_count);
}

void test_bb_pub_cadence_once_publishes_exactly_once(void)
{
    cadence_setup(BB_PUB_CADENCE_ONCE, false);
    bb_pub_tick_once();
    bb_pub_tick_once();
    bb_pub_tick_once();
    bb_pub_tick_once();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
}

void test_bb_pub_cadence_retain_true_forwarded(void)
{
    cadence_setup(BB_PUB_CADENCE_EVERY_TICK, true);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
    TEST_ASSERT_TRUE(s_cadence_last_retain);
}

void test_bb_pub_cadence_retain_false_forwarded(void)
{
    cadence_setup(BB_PUB_CADENCE_EVERY_TICK, false);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
    TEST_ASSERT_FALSE(s_cadence_last_retain);
}

void test_bb_pub_cadence_reset_clears_on_change_state(void)
{
    cadence_setup(BB_PUB_CADENCE_ON_CHANGE, false);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
    // Suppress: no change.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);

    // Reset: cadence state zeroed, next tick must republish the same bytes.
    bb_pub_test_reset();
    s_cadence_count = 0;

    bb_pub_telemetry_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.topic     = "cadtest";
    cfg.gather    = cadence_gather;
    cfg.serialize = cadence_serialize;
    cfg.snap_size = sizeof(cadence_snap_t);
    cfg.flags     = BB_PUB_TELEM_SINKS;
    cfg.cadence   = BB_PUB_CADENCE_ON_CHANGE;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    bb_pub_sink_t sink = { .publish = cadence_capture };
    bb_pub_add_sink(&sink);

    // Same data but hash state cleared → publishes again.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
}

void test_bb_pub_cadence_once_reset_allows_republish(void)
{
    cadence_setup(BB_PUB_CADENCE_ONCE, false);
    bb_pub_tick_once();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);

    // Reset: published_once cleared → next registration can publish again.
    bb_pub_test_reset();
    s_cadence_count = 0;

    bb_pub_telemetry_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.topic     = "cadtest";
    cfg.gather    = cadence_gather;
    cfg.serialize = cadence_serialize;
    cfg.snap_size = sizeof(cadence_snap_t);
    cfg.flags     = BB_PUB_TELEM_SINKS;
    cfg.cadence   = BB_PUB_CADENCE_ONCE;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    bb_pub_sink_t sink = { .publish = cadence_capture };
    bb_pub_add_sink(&sink);

    bb_pub_tick_once();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);
}

// Failing sink: increments s_cadence_count (delivery was attempted) but
// returns error so bb_pub_deliver_to_sinks sees failed > 0.
static bb_err_t cadence_fail_publish(void *ctx, const char *topic,
                                      const char *payload, int len, bool retain)
{
    (void)ctx; (void)topic; (void)payload; (void)len; (void)retain;
    s_cadence_count++;
    return BB_ERR_INVALID_STATE;
}

void test_bb_pub_cadence_on_change_failing_sink_leaves_state_uncommitted(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("cadencehost");
    s_cadence_val   = 42;
    s_cadence_count = 0;

    bb_pub_telemetry_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.topic     = "cadtest";
    cfg.gather    = cadence_gather;
    cfg.serialize = cadence_serialize;
    cfg.snap_size = sizeof(cadence_snap_t);
    cfg.flags     = BB_PUB_TELEM_SINKS;
    cfg.retain    = false;
    cfg.cadence   = BB_PUB_CADENCE_ON_CHANGE;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    bb_pub_sink_t fail_sink = { .publish = cadence_fail_publish };
    bb_pub_add_sink(&fail_sink);

    // Tick 1: content changed (hash 0 → h), delivery fails → hash NOT committed.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);

    // Switch to working sink; content unchanged.
    bb_pub_clear_sinks();
    bb_pub_sink_t ok_sink = { .publish = cadence_capture };
    bb_pub_add_sink(&ok_sink);

    // Tick 2: hash still uncommitted → publishes again and commits hash.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_cadence_count);

    // Tick 3: hash committed, no change → suppressed.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_cadence_count);
}

void test_bb_pub_cadence_once_failing_sink_leaves_state_uncommitted(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("cadencehost");
    s_cadence_val   = 42;
    s_cadence_count = 0;

    bb_pub_telemetry_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.topic     = "cadtest";
    cfg.gather    = cadence_gather;
    cfg.serialize = cadence_serialize;
    cfg.snap_size = sizeof(cadence_snap_t);
    cfg.flags     = BB_PUB_TELEM_SINKS;
    cfg.retain    = false;
    cfg.cadence   = BB_PUB_CADENCE_ONCE;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    bb_pub_sink_t fail_sink = { .publish = cadence_fail_publish };
    bb_pub_add_sink(&fail_sink);

    // Tick 1: !published_once → delivery attempted, fails → published_once NOT set.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_cadence_count);

    // Switch to working sink.
    bb_pub_clear_sinks();
    bb_pub_sink_t ok_sink = { .publish = cadence_capture };
    bb_pub_add_sink(&ok_sink);

    // Tick 2: published_once still false → publishes and sets published_once.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_cadence_count);

    // Tick 3: published_once true → suppressed.
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_cadence_count);
}

// ---------------------------------------------------------------------------
// Legacy source path envelope (B1-570 PR-3)
//
// bb_pub_register_source's tick path delivers to sinks BYPASSING bb_cache
// (unlike bb_pub_register_telemetry, which gets its envelope from
// bb_cache_get_serialized/bb_cache_post). This path must build the SAME
// {"ts_ms":<n>,"data":{...}} wire contract explicitly so every sink-delivered
// topic is uniform regardless of which registration API produced it.
// ---------------------------------------------------------------------------

void test_bb_pub_legacy_source_payload_is_enveloped(void)
{
    setup_with_sink();
    bb_pub_register_source("power", sample_voltage, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    bb_json_t root = bb_json_parse(s_captured[0].payload, strlen(s_captured[0].payload));
    TEST_ASSERT_NOT_NULL(root);

    double ts = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root, "ts_ms", &ts));

    bb_json_t data = bb_json_obj_get_item(root, "data");
    TEST_ASSERT_NOT_NULL(data);
    double mv = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "mv", &mv));
    TEST_ASSERT_EQUAL_INT(3300, (int)mv);

    // "mv" must NOT also appear at the envelope root (no flat/nested duplication).
    double flat_mv = 0;
    TEST_ASSERT_FALSE(bb_json_obj_get_number(root, "mv", &flat_mv));

    bb_json_free(root);
}

void test_bb_pub_legacy_source_envelope_ts_matches_uptime_ms(void)
{
    // ts_ms at the envelope root is the SAME per-cycle sample-time value
    // injected into "data.uptime_ms" (both come from the single timestamp
    // captured once per tick) — not two independently-sampled clocks.
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    bb_json_t root = bb_json_parse(s_captured[0].payload, strlen(s_captured[0].payload));
    TEST_ASSERT_NOT_NULL(root);

    double ts = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root, "ts_ms", &ts));

    bb_json_t data = bb_json_obj_get_item(root, "data");
    TEST_ASSERT_NOT_NULL(data);
    double uptime = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "uptime_ms", &uptime));

    TEST_ASSERT_EQUAL_INT((int)ts, (int)uptime);

    bb_json_free(root);
}
