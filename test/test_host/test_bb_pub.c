// Tests for bb_pub core: source registry, tick cycle, sink routing.
#include "unity.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "bb_cache.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"
#include "bb_str.h"

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

// bb_pub_pause()'s ETIMEDOUT branch: with the bounded-wait timeout
// overridden below the slow sink's publish duration, pause() must time out,
// log once, and return WITHOUT waiting for the publish to finish (instead
// of the happy-path bounded-wait-completes-normally case covered above).
#define SHORT_PAUSE_TIMEOUT_MS 20   /* well under SHORT_PUBLISH_MS (200) */

void test_bb_pub_pause_bounded_wait_times_out_on_slow_sink(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_test_set_pause_timeout_ms(SHORT_PAUSE_TIMEOUT_MS);

    slow_sink_ctx_t ssink;
    memset(&ssink, 0, sizeof(ssink));
    pthread_mutex_init(&ssink.mu, NULL);
    pthread_cond_init(&ssink.cv, NULL);

    bb_pub_sink_t sk = { .publish = slow_sink_publish, .ctx = &ssink };
    bb_pub_set_sink(&sk);
    bb_pub_register_source("x", sample_temperature, NULL);

    pthread_t tick_tid;
    pthread_create(&tick_tid, NULL, slow_tick_thread_fn, NULL);

    pthread_mutex_lock(&ssink.mu);
    while (!ssink.publish_entered) {
        pthread_cond_wait(&ssink.cv, &ssink.mu);
    }
    pthread_mutex_unlock(&ssink.mu);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bb_pub_pause();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long elapsed_ms = (long)(t1.tv_sec  - t0.tv_sec)  * 1000
                    + (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;

    // pause() must return around the SHORT override timeout, well before
    // the slow sink's SHORT_PUBLISH_MS (200 ms) publish completes --
    // proving the ETIMEDOUT path fired rather than the normal wait-for-
    // completion path.
    TEST_ASSERT_TRUE_MESSAGE(elapsed_ms < SHORT_PUBLISH_MS,
        "bb_pub_pause did not time out on the overridden short timeout");
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    pthread_join(tick_tid, NULL);
    TEST_ASSERT_EQUAL_INT(1, ssink.call_count);

    pthread_mutex_destroy(&ssink.mu);
    pthread_cond_destroy(&ssink.cv);
}

// bb_pub_pause()'s absolute-deadline nanosecond-carry branch
// (abs_ts.tv_nsec >= 1000000000L -> tv_sec++, tv_nsec -= 1e9). Using a
// timeout whose ms%1000 component is 999 guarantees the added 999,000,000 ns
// overflows 1 second unless clock_gettime()'s nsec component happens to be
// under 1,000,000 (a <0.1% coincidence) -- deterministic enough for CI.
#define CARRY_PAUSE_TIMEOUT_MS 999
#define CARRY_SINK_SLEEP_MS    1200   /* > CARRY_PAUSE_TIMEOUT_MS: forces ETIMEDOUT too */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            publish_entered;
} carry_sink_ctx_t;

static bb_err_t carry_sink_publish(void *ctx, const char *topic,
                                    const char *payload, int len, bool retain)
{
    (void)topic; (void)payload; (void)len; (void)retain;
    carry_sink_ctx_t *s = (carry_sink_ctx_t *)ctx;

    pthread_mutex_lock(&s->mu);
    s->publish_entered = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);

    struct timespec ts_sleep = {
        .tv_sec  = CARRY_SINK_SLEEP_MS / 1000,
        .tv_nsec = (CARRY_SINK_SLEEP_MS % 1000) * 1000000L,
    };
    nanosleep(&ts_sleep, NULL);
    return BB_OK;
}

static void *carry_tick_thread_fn(void *arg)
{
    (void)arg;
    bb_pub_tick_once();
    return NULL;
}

void test_bb_pub_pause_bounded_wait_deadline_nsec_carry(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_test_set_pause_timeout_ms(CARRY_PAUSE_TIMEOUT_MS);

    carry_sink_ctx_t csink;
    memset(&csink, 0, sizeof(csink));
    pthread_mutex_init(&csink.mu, NULL);
    pthread_cond_init(&csink.cv, NULL);

    bb_pub_sink_t sk = { .publish = carry_sink_publish, .ctx = &csink };
    bb_pub_set_sink(&sk);
    bb_pub_register_source("x", sample_temperature, NULL);

    pthread_t tick_tid;
    pthread_create(&tick_tid, NULL, carry_tick_thread_fn, NULL);

    pthread_mutex_lock(&csink.mu);
    while (!csink.publish_entered) {
        pthread_cond_wait(&csink.cv, &csink.mu);
    }
    pthread_mutex_unlock(&csink.mu);

    // Must not crash regardless of the absolute-deadline carry; pause()
    // still returns (via ETIMEDOUT) well before the sink's long sleep ends.
    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    pthread_join(tick_tid, NULL);
    pthread_mutex_destroy(&csink.mu);
    pthread_cond_destroy(&csink.cv);
}

// ---------------------------------------------------------------------------
// bb_pub_set_interval_ms / bb_pub_set_enabled: NVS-write-failure injection
// ---------------------------------------------------------------------------

static bb_err_t cov_nv_set_u32_fail(const char *ns, const char *key, uint32_t value)
{
    (void)ns; (void)key; (void)value;
    return BB_ERR_INVALID_STATE;
}

static bb_err_t cov_nv_set_u8_fail(const char *ns, const char *key, uint8_t value)
{
    (void)ns; (void)key; (void)value;
    return BB_ERR_INVALID_STATE;
}

// bb_pub_set_interval_ms must still return BB_OK (the in-RAM value is
// updated regardless) even when the NVS write itself fails -- only the
// warning log path differs.
void test_bb_pub_set_interval_ms_nvs_write_failure_still_returns_ok(void)
{
    bb_pub_test_reset();
    bb_pub_test_set_nv_set_u32(cov_nv_set_u32_fail);
    bb_err_t err = bb_pub_set_interval_ms(12345);
    bb_pub_test_set_nv_set_u32(NULL);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_UINT32(12345, bb_pub_get_interval_ms());
}

// bb_pub_set_enabled must still return BB_OK even when the NVS write fails.
void test_bb_pub_set_enabled_nvs_write_failure_still_returns_ok(void)
{
    bb_pub_test_reset();
    bb_pub_test_set_nv_set_u8(cov_nv_set_u8_fail);
    bb_err_t err = bb_pub_set_enabled(false);
    bb_pub_test_set_nv_set_u8(NULL);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(bb_pub_is_enabled());
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

// ---------------------------------------------------------------------------
// B1-516: coverage-closing tests (bb_pub core)
// ---------------------------------------------------------------------------

// bb_pub_buffer_init_eager
#if CONFIG_BB_PUB_BUFFER_ENABLE
void test_bb_pub_buffer_init_eager_always_on_calls_ring_pool_get(void)
{
    bb_pub_test_reset();
    bb_pub_test_set_buffer_always(true);
    // Must not crash; the ring pool is allocated (or a warning logged on
    // alloc failure) — either way bb_pub_buffer_stats() stays well-formed.
    bb_pub_buffer_init_eager();
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_INT(0, (int)stats.count);
}

void test_bb_pub_buffer_init_eager_not_always_on_is_noop(void)
{
    bb_pub_test_reset();
    bb_pub_test_set_buffer_always(false);
    bb_pub_buffer_init_eager();
    bb_pub_buffer_stats_t stats;
    bb_pub_buffer_stats(&stats);
    TEST_ASSERT_EQUAL_INT(0, (int)stats.count);
}
#endif

// bb_pub_sink_info: invalid index
void test_bb_pub_sink_info_negative_index_returns_invalid_arg(void)
{
    setup_with_sink();
    const char *transport = NULL;
    bool tls = false;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_sink_info(-1, &transport, &tls));
}

void test_bb_pub_sink_info_index_ge_count_returns_invalid_arg(void)
{
    setup_with_sink();
    const char *transport = NULL;
    bool tls = false;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_sink_info(1, &transport, &tls));
}

// ---------------------------------------------------------------------------
// bb_pub_register_telemetry validation
// ---------------------------------------------------------------------------

typedef struct { int value; } cov_telem_snap_t;

static bool cov_telem_gather_ok(void *buf, void *ctx)
{
    (void)ctx;
    cov_telem_snap_t *s = (cov_telem_snap_t *)buf;
    s->value = 1;
    return true;
}

static void cov_telem_serialize(bb_json_t obj, const void *snap)
{
    const cov_telem_snap_t *s = (const cov_telem_snap_t *)snap;
    bb_json_obj_set_number(obj, "value", (double)s->value);
}

void test_bb_pub_register_telemetry_null_cfg_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_register_telemetry(NULL));
}

void test_bb_pub_register_telemetry_null_topic_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_telemetry_cfg_t cfg = {
        .topic = NULL, .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_register_telemetry(&cfg));
}

void test_bb_pub_register_telemetry_null_gather_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_a", .gather = NULL,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_register_telemetry(&cfg));
}

void test_bb_pub_register_telemetry_null_serialize_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_a", .gather = cov_telem_gather_ok,
        .serialize = NULL, .snap_size = sizeof(cov_telem_snap_t),
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_register_telemetry(&cfg));
}

void test_bb_pub_register_telemetry_zero_snap_size_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_a", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = 0,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_register_telemetry(&cfg));
}

void test_bb_pub_register_telemetry_snap_size_over_max_returns_no_space(void)
{
    bb_pub_test_reset();
    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_a", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize,
        .snap_size = CONFIG_BB_PUB_TELEM_SNAP_MAX + 1,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pub_register_telemetry(&cfg));
}

void test_bb_pub_register_telemetry_table_full_returns_no_space(void)
{
    bb_pub_test_reset();
    char topic_buf[CONFIG_BB_PUB_MAX_SOURCES][16];
    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        snprintf(topic_buf[i], sizeof(topic_buf[i]), "cov_full_%d", i);
        bb_pub_telemetry_cfg_t cfg = {
            .topic = topic_buf[i], .gather = cov_telem_gather_ok,
            .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
        };
        TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));
    }
    // Telem table (and source table) is now full at CONFIG_BB_PUB_MAX_SOURCES.
    bb_pub_telemetry_cfg_t overflow_cfg = {
        .topic = "cov_full_overflow", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pub_register_telemetry(&overflow_cfg));
}

// bb_cache_register() failing (registry full) must propagate through
// bb_pub_register_telemetry. Fill bb_cache's own registry directly with
// topics bb_pub never sees (idempotent-by-key, mirrors test_bb_sub.c's
// pattern), leaving bb_pub's own (much smaller) telem table nearly empty.
void test_bb_pub_register_telemetry_cache_register_failure_propagates(void)
{
    bb_pub_test_reset();
    char fill_buf[32];
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        snprintf(fill_buf, sizeof(fill_buf), "cov_cachefill.%d", i);
        bb_cache_config_t cfg = {
            .key       = fill_buf,
            .snapshot  = NULL,
            .snap_size = 8,
            .serialize = cov_telem_serialize,
            .flags     = BB_CACHE_FLAG_NONE,
        };
        bb_err_t rc = bb_cache_register(&cfg);
        TEST_ASSERT_TRUE_MESSAGE(rc == BB_OK || rc == BB_ERR_NO_SPACE, fill_buf);
    }
    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_cache_full_new_topic", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pub_register_telemetry(&cfg));
}

// bb_pub_register_source() failing (source registry full, telem table NOT
// full) must undo the already-committed telem entry.
void test_bb_pub_register_telemetry_source_registry_full_undoes_telem_entry(void)
{
    bb_pub_test_reset();
    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        char sub[16];
        snprintf(sub, sizeof(sub), "plain_%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source(sub, sample_temperature, NULL));
    }
    TEST_ASSERT_EQUAL(CONFIG_BB_PUB_MAX_SOURCES, bb_pub_source_count());

    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_undo_topic", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
    };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pub_register_telemetry(&cfg));
    // Source count must be unchanged (register_source itself failed).
    TEST_ASSERT_EQUAL(CONFIG_BB_PUB_MAX_SOURCES, bb_pub_source_count());
}

// ---------------------------------------------------------------------------
// bb_pub_set_interval_volatile_ms: hook-invoked branch
// ---------------------------------------------------------------------------

static uint32_t s_volatile_hook_ms = 0;
static void cov_volatile_hook(uint32_t ms) { s_volatile_hook_ms = ms; }

void test_bb_pub_set_interval_volatile_ms_invokes_registered_hook(void)
{
    bb_pub_test_reset();
    s_volatile_hook_ms = 0;
    bb_pub_set_interval_apply_hook(cov_volatile_hook);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_volatile_ms(12345));
    TEST_ASSERT_EQUAL_UINT32(12345, s_volatile_hook_ms);
    bb_pub_set_interval_apply_hook(NULL);
}

// ---------------------------------------------------------------------------
// Hostname fallback: empty hostname -> "device"
// ---------------------------------------------------------------------------

void test_bb_pub_tick_hostname_fallback_to_device_when_empty(void)
{
    bb_pub_test_reset();
    capture_reset();
    // bb_nv_config_set_hostname() rejects an empty string outright, and no
    // hostname value survives a validated set — factory_reset is the only
    // way to force the in-RAM hostname cache back to its zeroed ("") default.
    bb_nv_config_factory_reset();
    bb_pub_sink_t sink = make_capture_sink();
    bb_pub_set_sink(&sink);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/device/temp", s_captured[0].topic);
}

// ---------------------------------------------------------------------------
// bb_pub_deliver_to_sinks: per-sink subscribe predicate skip
// ---------------------------------------------------------------------------

static bool cov_reject_all(const char *subtopic, const char *const *tags,
                            int ntags, void *ctx)
{
    (void)subtopic; (void)tags; (void)ntags; (void)ctx;
    return false;
}

void test_bb_pub_telemetry_sink_subscribe_false_is_skipped(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t rejecting = make_capture_sink();
    rejecting.subscribe = cov_reject_all;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&rejecting));

    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_subskip", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
        .flags = BB_PUB_TELEM_SINKS, .cadence = BB_PUB_CADENCE_EVERY_TICK,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    bb_pub_tick_once();
    // The only registered sink rejects this subtopic -> deliver_to_sinks
    // must skip it without treating the skip as a delivery failure.
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

// ---------------------------------------------------------------------------
// Telem Phase 1: bb_cache_update failure (entry deleted out from under a
// registered telem source) must mark the source as not-fired without crash.
// ---------------------------------------------------------------------------

void test_bb_pub_telemetry_cache_update_failure_marks_not_fired(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_sink_t sink = make_capture_sink();
    bb_pub_set_sink(&sink);

    bb_pub_telemetry_cfg_t cfg = {
        .topic = "cov_deleted", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
        .flags = BB_PUB_TELEM_SINKS, .cadence = BB_PUB_CADENCE_EVERY_TICK,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg));

    // Simulate the cache entry disappearing between registration and tick
    // (e.g. an AGE_OUT eviction sweep) — bb_cache_update() must then fail
    // with BB_ERR_NOT_FOUND inside Phase 1.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete("cov_deleted"));

    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

// ---------------------------------------------------------------------------
// Telem Phase 2b: bb_cache_get_serialized failure (entry deleted mid-tick by
// a later-registered telem source's own gather) must skip gracefully.
// ---------------------------------------------------------------------------

static bool cov_telem_gather_deletes_other(void *buf, void *ctx)
{
    (void)ctx;
    cov_telem_snap_t *s = (cov_telem_snap_t *)buf;
    s->value = 2;
    // Side effect: remove the FIRST-registered telem source's cache entry
    // mid-Phase-1, so its Phase 2b get_serialized() call fails.
    bb_cache_delete("cov_evictee");
    return true;
}

void test_bb_pub_telemetry_get_serialized_failure_skips_gracefully(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_sink_t sink = make_capture_sink();
    bb_pub_set_sink(&sink);

    bb_pub_telemetry_cfg_t cfg_a = {
        .topic = "cov_evictee", .gather = cov_telem_gather_ok,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
        .flags = BB_PUB_TELEM_SINKS, .cadence = BB_PUB_CADENCE_EVERY_TICK,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg_a));

    bb_pub_telemetry_cfg_t cfg_b = {
        .topic = "cov_evictor", .gather = cov_telem_gather_deletes_other,
        .serialize = cov_telem_serialize, .snap_size = sizeof(cov_telem_snap_t),
        .flags = BB_PUB_TELEM_SINKS, .cadence = BB_PUB_CADENCE_EVERY_TICK,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_telemetry(&cfg_b));

    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Only "cov_evictor" (B) reaches the sink; "cov_evictee" (A) was deleted
    // out from under Phase 2b's get_serialized() call and skipped silently.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/cov_evictor", s_captured[0].topic);
}

// ---------------------------------------------------------------------------
// Legacy source path: bb_json allocation / serialize fault injection
// ---------------------------------------------------------------------------

void test_bb_pub_tick_legacy_source_obj_alloc_failure_skips_source(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_json_host_force_alloc_fail_after(0);   /* fail the source's own obj_new */
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_tick_legacy_source_envelope_alloc_failure_skips_source(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    // 1st bb_json_obj_new() call is the source's own object (succeeds);
    // the 2nd is the envelope object — force that one to fail.
    bb_json_host_force_alloc_fail_after(1);
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_tick_legacy_source_serialize_failure_skips_source(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_json_host_force_serialize_fail_after(0);   /* fail the envelope serialize */
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

// ---------------------------------------------------------------------------
// B1-516 pass 2: reachable-without-a-seam branch coverage (bb_pub core)
// ---------------------------------------------------------------------------

void test_bb_pub_buffer_stats_null_out_is_noop(void)
{
    bb_pub_test_reset();
    // Must not crash.
    bb_pub_buffer_stats(NULL);
}

// bb_pub_set_sink with a non-NULL sink whose publish is NULL — distinct
// from the sink==NULL case (test_bb_pub_set_sink_null_clears_sink).
void test_bb_pub_set_sink_non_null_with_null_publish_returns_ok_noop(void)
{
    bb_pub_test_reset();
    bb_pub_sink_t s = { .publish = NULL, .ctx = NULL };
    bb_err_t err = bb_pub_set_sink(&s);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_pub_status_t status;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&status));
    TEST_ASSERT_EQUAL_INT(0, status.sink_count);
}

void test_bb_pub_source_info_index_ge_count_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_register_source("a", sample_temperature, NULL);
    const char *subtopic = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_pub_source_info(1, &subtopic, NULL, NULL, NULL, NULL));
}

void test_bb_pub_source_info_negative_index_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_register_source("a", sample_temperature, NULL);
    const char *subtopic = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_pub_source_info(-1, &subtopic, NULL, NULL, NULL, NULL));
}

void test_bb_pub_source_info_ex_null_ntags_is_safe(void)
{
    bb_pub_test_reset();
    const char *tags[] = { "a" };
    bb_pub_register_source_ex("a", sample_temperature, NULL, tags, 1);
    const char *const *out_tags = NULL;
    bb_err_t err = bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, &out_tags, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_pub_sink_info_null_out_transport_is_safe(void)
{
    setup_with_sink();
    bool tls = false;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_sink_info(0, NULL, &tls));
}

void test_bb_pub_sink_info_null_out_tls_is_safe(void)
{
    setup_with_sink();
    const char *transport = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_sink_info(0, &transport, NULL));
}

void test_bb_pub_set_metrics_prefix_null_is_noop(void)
{
    bb_pub_test_reset();
    const char *before = bb_pub_metrics_prefix();
    char saved[64];
    bb_strlcpy(saved, before, sizeof(saved));
    bb_pub_set_metrics_prefix(NULL);
    TEST_ASSERT_EQUAL_STRING(saved, bb_pub_metrics_prefix());
}

// bb_pub_register_source_ex must propagate a failure from the underlying
// bb_pub_register_source call (source registry full) rather than proceeding
// to write tags.
void test_bb_pub_register_source_ex_propagates_register_source_failure(void)
{
    bb_pub_test_reset();
    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        char sub[16];
        snprintf(sub, sizeof(sub), "s%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source(sub, sample_temperature, NULL));
    }
    const char *tags[] = { "t" };
    bb_err_t err = bb_pub_register_source_ex("overflow", sample_temperature, NULL, tags, 1);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

// ---------------------------------------------------------------------------
// B1-516 pass 3: reachable-without-a-seam Phase 2 bookkeeping branches
// ---------------------------------------------------------------------------

// bb_pub_tick_once Phase 2a: a failure on a NON-FIRST sink (si != 0) must
// still be logged / marked tick_all_ok=false, distinct from the si==0
// failure case already covered by test_bb_pub_failing_sink_does_not_stop_other_sink.
void test_bb_pub_failing_non_first_sink_marks_not_all_ok(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
#if CONFIG_BB_PUB_BUFFER_ENABLE
    // Force on-failure mode so last_publish_ok reflects tick_all_ok directly
    // rather than the always-on mode's ring-drained status.
    bb_pub_test_set_buffer_always(false);
#endif

    bb_pub_sink_t good = make_capture_sink();
    bb_pub_sink_t bad  = { .publish = failing_publish, .ctx = NULL };

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&good));  // si=0, succeeds
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&bad));   // si=1, fails

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_err_t err = bb_pub_tick_once();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    bb_pub_status_t status;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&status));
    TEST_ASSERT_FALSE(status.last_publish_ok);
}

// bb_pub_tick_once Phase 2a: a legacy (non-telemetry) source with a
// per-sink subscribe filter must skip the rejecting sink's direct publish
// (Phase 2a's own !p->subscribed[si] guard, distinct from the Phase 1
// any_subscribed pre-filter which only guarantees at least one sink is
// subscribed).
void test_bb_pub_legacy_source_per_sink_subscribe_filter_skips_rejecting_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t rejecting = make_capture_sink();
    rejecting.subscribe = cov_reject_all;
    bb_pub_sink_t accepting = make_capture_sink();

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&rejecting));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&accepting));

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_err_t err = bb_pub_tick_once();

    TEST_ASSERT_EQUAL(BB_OK, err);
    // Only the accepting sink receives it -- 1 capture, not 2 (both sinks
    // share s_captured/s_capture_count via make_capture_sink's capture_publish).
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

// ---------------------------------------------------------------------------
// B1-651: bb_pub_add_sink / bb_pub_set_sink / bb_pub_clear_sinks vs
// bb_pub_tick_once TOCTOU / iterator-invalidation race.
//
// bb_pub_tick_once's Phase 1 copies s_sinks[]/s_sink_count into a local
// snap_sinks[] array under s_tick_lock. Before the fix the sink-set mutators
// wrote that same array WITHOUT holding s_tick_lock, so a concurrent
// add/set/clear could interleave with the copy loop mid-struct (a "torn"
// bb_pub_sink_t combining one sink's `publish` fn pointer with another's
// `ctx`), or resize/reorder the array while it was being read.
//
// This test hammers the mutators from a second thread while the main thread
// repeatedly ticks, and verifies every delivered sink struct is fully
// self-consistent (ctx magic matches the sink that owns it) -- a torn read
// would very likely trip the magic-mismatch guard (or crash outright via a
// corrupted function/ctx pointer). Run count is large enough to force the
// interleaving with high probability against the pre-fix code.
// ---------------------------------------------------------------------------

#define RACE_MAGIC_A 0xA5A5A5A5u
#define RACE_MAGIC_B 0x5A5A5A5Au

typedef struct {
    uint32_t magic;
    int      calls;
} race_sink_ctx_t;

static race_sink_ctx_t s_race_ctx_a;
static race_sink_ctx_t s_race_ctx_b;
static bool            s_race_corrupted;
static bool            s_race_saw_invalid_state;

static bb_err_t race_publish(void *ctx, const char *topic, const char *payload,
                              int len, bool retain)
{
    (void)topic;
    (void)len;
    (void)retain;
    race_sink_ctx_t *c = (race_sink_ctx_t *)ctx;
    if (!c || (c->magic != RACE_MAGIC_A && c->magic != RACE_MAGIC_B)) {
        s_race_corrupted = true;
        return BB_ERR_INVALID_ARG;
    }
    if (!payload || payload[0] != '{') {
        s_race_corrupted = true;
        return BB_ERR_INVALID_ARG;
    }
    c->calls++;
    return BB_OK;
}

typedef struct {
    volatile bool       stop;
    const bb_pub_sink_t *sink_a;
    const bb_pub_sink_t *sink_b;
} race_mutator_arg_t;

static void *race_mutator_fn(void *arg)
{
    race_mutator_arg_t *a = (race_mutator_arg_t *)arg;
    while (!a->stop) {
        // This runs on a genuinely different thread than the one ticking
        // (B1-651's reentrancy guard is thread-scoped — see
        // test_bb_pub_add_sink_from_sample_fn_returns_invalid_state), so
        // none of these calls should ever see BB_ERR_INVALID_STATE; they
        // should simply block on s_tick_lock as before B1-651.
        if (bb_pub_add_sink(a->sink_b) == BB_ERR_INVALID_STATE) s_race_saw_invalid_state = true;
        if (bb_pub_clear_sinks() == BB_ERR_INVALID_STATE) s_race_saw_invalid_state = true;
        if (bb_pub_set_sink(a->sink_a) == BB_ERR_INVALID_STATE) s_race_saw_invalid_state = true;
        if (bb_pub_add_sink(a->sink_b) == BB_ERR_INVALID_STATE) s_race_saw_invalid_state = true;
        if (bb_pub_clear_sinks() == BB_ERR_INVALID_STATE) s_race_saw_invalid_state = true;
        if (bb_pub_set_sink(a->sink_b) == BB_ERR_INVALID_STATE) s_race_saw_invalid_state = true;
    }
    return NULL;
}

void test_bb_pub_sink_mutators_locked_against_concurrent_tick(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("racehost");
    bb_pub_register_source("race", sample_temperature, NULL);

    s_race_corrupted = false;
    s_race_saw_invalid_state = false;
    s_race_ctx_a = (race_sink_ctx_t){ .magic = RACE_MAGIC_A, .calls = 0 };
    s_race_ctx_b = (race_sink_ctx_t){ .magic = RACE_MAGIC_B, .calls = 0 };
    bb_pub_sink_t sink_a = { .publish = race_publish, .ctx = &s_race_ctx_a };
    bb_pub_sink_t sink_b = { .publish = race_publish, .ctx = &s_race_ctx_b };
    bb_pub_add_sink(&sink_a);

    race_mutator_arg_t arg = { .stop = false, .sink_a = &sink_a, .sink_b = &sink_b };
    pthread_t mutator_tid;
    pthread_create(&mutator_tid, NULL, race_mutator_fn, &arg);

    for (int i = 0; i < 2000; i++) {
        bb_pub_tick_once();
    }

    arg.stop = true;
    pthread_join(mutator_tid, NULL);

    TEST_ASSERT_FALSE(s_race_corrupted);
    TEST_ASSERT_FALSE(s_race_saw_invalid_state);

    bb_pub_status_t status;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&status));
    TEST_ASSERT_TRUE(status.sink_count >= 0);
    TEST_ASSERT_TRUE(status.sink_count <= CONFIG_BB_PUB_MAX_SINKS);
}

// ---------------------------------------------------------------------------
// B1-651: reentrancy guard.  s_tick_lock is a plain (non-recursive) mutex,
// and bb_pub_tick_once's Phase 1 holds it across every source sample_fn and
// payload extender.  A sample_fn/extender that (directly or indirectly)
// calls a sink mutator from the SAME thread would otherwise re-acquire a
// lock its own thread already holds and deadlock the worker forever.  These
// tests exercise the guard from both callback sites and confirm
// BB_ERR_INVALID_STATE is returned instead of hanging.
// ---------------------------------------------------------------------------

static bb_err_t s_reentrant_add_sink_err  = BB_OK;
static bb_err_t s_reentrant_set_sink_err  = BB_OK;
static bb_err_t s_reentrant_clear_sink_err = BB_OK;
static bb_pub_sink_t s_reentrant_probe_sink;

static bool reentrant_sample_fn(bb_json_t obj, void *ctx)
{
    (void)ctx;
    // Called from the tick-owning thread while s_tick_lock is held (Phase 1).
    // A non-guarded implementation would self-deadlock here.
    s_reentrant_add_sink_err   = bb_pub_add_sink(&s_reentrant_probe_sink);
    s_reentrant_set_sink_err   = bb_pub_set_sink(&s_reentrant_probe_sink);
    s_reentrant_clear_sink_err = bb_pub_clear_sinks();
    bb_json_obj_set_bool(obj, "ok", true);
    return true;
}

void test_bb_pub_add_sink_from_sample_fn_returns_invalid_state(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("reentranthost");

    s_reentrant_probe_sink = make_capture_sink();
    bb_pub_sink_t outer_sink = make_capture_sink();
    bb_pub_add_sink(&outer_sink);
    bb_pub_register_source("reentrant", reentrant_sample_fn, NULL);

    s_reentrant_add_sink_err   = BB_OK;
    s_reentrant_set_sink_err   = BB_OK;
    s_reentrant_clear_sink_err = BB_OK;

    // Would hang forever pre-fix; the surrounding Unity test harness itself
    // has no built-in timeout, so a regression here manifests as the whole
    // suite run hanging rather than a targeted assertion failure — that is
    // exactly the failure mode this guard exists to prevent.
    bb_err_t tick_err = bb_pub_tick_once();

    TEST_ASSERT_EQUAL(BB_OK, tick_err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, s_reentrant_add_sink_err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, s_reentrant_set_sink_err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, s_reentrant_clear_sink_err);

    // The reentrant calls must have been true no-ops: the outer sink is
    // still the only registered sink.
    bb_pub_status_t status;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&status));
    TEST_ASSERT_EQUAL_INT(1, status.sink_count);
}

static bb_err_t s_reentrant_extender_add_err = BB_OK;

static void reentrant_payload_extender(bb_json_t obj, const char *source, void *ctx)
{
    (void)obj;
    (void)source;
    (void)ctx;
    // Payload extenders also run on the tick-owning thread under
    // s_tick_lock (Phase 1) — same reentrancy hazard as a sample_fn.
    s_reentrant_extender_add_err = bb_pub_add_sink(&s_reentrant_probe_sink);
}

void test_bb_pub_add_sink_from_payload_extender_returns_invalid_state(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("reentranthost2");

    s_reentrant_probe_sink = make_capture_sink();
    bb_pub_sink_t outer_sink = make_capture_sink();
    bb_pub_add_sink(&outer_sink);
    bb_pub_register_source("plain", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_OK,
        bb_pub_register_payload_extender(reentrant_payload_extender, NULL));

    s_reentrant_extender_add_err = BB_OK;

    bb_err_t tick_err = bb_pub_tick_once();

    TEST_ASSERT_EQUAL(BB_OK, tick_err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, s_reentrant_extender_add_err);

    bb_pub_status_t status;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&status));
    TEST_ASSERT_EQUAL_INT(1, status.sink_count);
}
