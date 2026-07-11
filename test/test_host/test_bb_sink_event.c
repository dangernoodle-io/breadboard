// Tests for bb_sink_event: event-bus sink adapter.
#include "unity.h"
#include "bb_sink_event.h"
#include "bb_pub.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_event_test.h"
#include "test_hostname_seed.h"
#include "bb_json.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Forward decl — avoids pulling bb_event_routes_internal.h into test code.
extern void bb_event_routes_reset_for_test(void);

// ---------------------------------------------------------------------------
// Sample helpers
// ---------------------------------------------------------------------------

static bool sample_power(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "vout_mv", 1200.0);
    return true;
}

static bool sample_skip(bb_json_t obj, void *ctx)
{
    (void)obj;
    (void)ctx;
    return false;
}

// ---------------------------------------------------------------------------
// Event subscriber spy
// ---------------------------------------------------------------------------

static int s_received_count = 0;
static char s_received_payload[512];

static void spy_handler(bb_event_topic_t topic, int32_t id,
                        const void *data, size_t len, void *user)
{
    (void)topic;
    (void)id;
    (void)user;
    s_received_count++;
    if (data && len > 0 && len < sizeof(s_received_payload)) {
        memcpy(s_received_payload, data, len);
        s_received_payload[len] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Per-test init helper (global setUp already resets pub/event/routes state)
// ---------------------------------------------------------------------------

static void init_event_system(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    bb_event_init(NULL);
    bb_event_routes_init(NULL);
    bb_sink_event_reset_for_test();
    s_received_count = 0;
    s_received_payload[0] = '\0';
    bb_test_seed_hostname("testhost");
}

// ---------------------------------------------------------------------------
// bb_sink_event tests
// ---------------------------------------------------------------------------

void test_bb_sink_event_null_out_returns_invalid_arg(void)
{
    init_event_system();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_sink_event(NULL));
}

void test_bb_sink_event_fills_sink(void)
{
    init_event_system();
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_event(&s));
    TEST_ASSERT_NOT_NULL(s.publish);
    TEST_ASSERT_NULL(s.transport);
    TEST_ASSERT_FALSE(s.tls);
}

void test_bb_sink_event_register_topic_null_returns_invalid_arg(void)
{
    init_event_system();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_sink_event_register_topic(NULL, false));
}

void test_bb_sink_event_register_topic_empty_returns_invalid_arg(void)
{
    init_event_system();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_sink_event_register_topic("", false));
}

void test_bb_sink_event_register_topic_ok(void)
{
    init_event_system();
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_event_register_topic("power", false));
}

void test_bb_sink_event_tick_delivers_to_event_topic(void)
{
    init_event_system();
    bb_sink_event_register_topic("power", false);

    bb_event_topic_t topic;
    bb_event_topic_lookup("power", &topic);
    bb_event_sub_t sub;
    bb_event_subscribe(topic, spy_handler, NULL, &sub);

    bb_pub_register_source("power", sample_power, NULL);
    bb_pub_sink_t s;
    bb_sink_event(&s);
    bb_pub_set_sink(&s);

    bb_pub_tick_once();
    bb_event_pump(10);

    TEST_ASSERT_EQUAL(1, s_received_count);
    TEST_ASSERT_NOT_NULL(strstr(s_received_payload, "vout_mv"));
}

void test_bb_sink_event_unregistered_subtopic_skipped(void)
{
    init_event_system();
    // Register "fan" but publish "power" — should be skipped.
    bb_sink_event_register_topic("fan", false);

    bb_event_topic_t topic;
    bb_event_topic_lookup("fan", &topic);
    bb_event_sub_t sub;
    bb_event_subscribe(topic, spy_handler, NULL, &sub);

    bb_pub_register_source("power", sample_power, NULL);
    bb_pub_sink_t s;
    bb_sink_event(&s);
    bb_pub_set_sink(&s);

    bb_pub_tick_once();
    bb_event_pump(10);

    TEST_ASSERT_EQUAL(0, s_received_count);
}

void test_bb_sink_event_sample_skip_not_delivered(void)
{
    init_event_system();
    bb_sink_event_register_topic("power", false);

    bb_event_topic_t topic;
    bb_event_topic_lookup("power", &topic);
    bb_event_sub_t sub;
    bb_event_subscribe(topic, spy_handler, NULL, &sub);

    bb_pub_register_source("power", sample_skip, NULL);
    bb_pub_sink_t s;
    bb_sink_event(&s);
    bb_pub_set_sink(&s);

    bb_pub_tick_once();
    bb_event_pump(10);

    TEST_ASSERT_EQUAL(0, s_received_count);
}

void test_bb_sink_event_seed_all_posts_snapshot(void)
{
    init_event_system();
    bb_sink_event_register_topic("power", true);
    bb_pub_register_source("power", sample_power, NULL);

    bb_event_topic_t topic;
    bb_event_topic_lookup("power", &topic);
    bb_event_sub_t sub;
    bb_event_subscribe(topic, spy_handler, NULL, &sub);

    bb_sink_event_seed_all();
    bb_event_pump(10);

    TEST_ASSERT_EQUAL(1, s_received_count);
    TEST_ASSERT_NOT_NULL(strstr(s_received_payload, "vout_mv"));
}

void test_bb_sink_event_seed_all_no_source_skips(void)
{
    init_event_system();
    // Register topic but no bb_pub source for it.
    bb_sink_event_register_topic("power", false);

    bb_event_topic_t topic;
    bb_event_topic_lookup("power", &topic);
    bb_event_sub_t sub;
    bb_event_subscribe(topic, spy_handler, NULL, &sub);

    bb_sink_event_seed_all();  // should not crash
    bb_event_pump(10);

    TEST_ASSERT_EQUAL(0, s_received_count);
}

void test_bb_sink_event_max_topics_returns_no_space(void)
{
    init_event_system();
    char name[32];
    for (int i = 0; i < BB_SINK_EVENT_MAX_TOPICS; i++) {
        snprintf(name, sizeof(name), "topic%d", i);
        bb_err_t err = bb_sink_event_register_topic(name, false);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_sink_event_register_topic("overflow", false));
}
