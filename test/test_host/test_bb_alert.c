#include "unity.h"
#include "bb_alert.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include "bb_event_test.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------

static int    s_event_count = 0;
static char   s_last_payload[512];
static size_t s_last_payload_size = 0;

static void capture_alert(bb_event_topic_t topic, int32_t id,
                           const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)user;
    s_event_count++;
    s_last_payload_size = size;
    if (data && size > 0 && size < sizeof(s_last_payload)) {
        memcpy(s_last_payload, data, size);
        s_last_payload[size] = '\0';
    }
}

static void reset_world(void)
{
    bb_alert_reset_for_test();
    bb_event_reset_for_test();
    bb_event_init(NULL);
    bb_alert_set_min_severity_for_test(BB_ALERT_INFO);
    s_event_count = 0;
    s_last_payload_size = 0;
    s_last_payload[0] = '\0';
}

// Fill callback that adds a "foo"="bar" field.
static void fill_foo_bar(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_string(obj, "foo", "bar");
}

// ---------------------------------------------------------------------------
// test: emit before init (no topic registered) must not crash
// ---------------------------------------------------------------------------

void test_bb_alert_emit_no_topic_noop(void)
{
    reset_world();
    // No bb_alert_register() called — s_topic is NULL.
    // Should silently return without crash.
    bb_alert_emit("test_event", BB_ALERT_INFO, NULL, NULL);
    // No ring to check — just verify no crash.
    TEST_PASS();
}

// ---------------------------------------------------------------------------
// test: emit below threshold is dropped (not delivered to ring)
// ---------------------------------------------------------------------------

void test_bb_alert_emit_below_threshold_dropped(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_alert_register());
    bb_event_topic_t topic = bb_alert_topic_for_test();
    TEST_ASSERT_NOT_NULL(topic);

    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_ring_attach_ex(topic, 4, 512, false, &ring));

    // Set threshold to WARNING — INFO should be dropped.
    bb_alert_set_min_severity_for_test(BB_ALERT_WARNING);

    bb_alert_emit("test_info", BB_ALERT_INFO, NULL, NULL);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(0, (int)bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// test: emit at threshold is delivered to ring
// ---------------------------------------------------------------------------

void test_bb_alert_emit_at_threshold_emitted(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_alert_register());
    bb_event_topic_t topic = bb_alert_topic_for_test();
    TEST_ASSERT_NOT_NULL(topic);

    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_ring_attach_ex(topic, 4, 512, false, &ring));

    // Threshold = INFO (0) — all severities pass.
    bb_alert_set_min_severity_for_test(BB_ALERT_INFO);

    bb_alert_emit("test_info", BB_ALERT_INFO, NULL, NULL);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, (int)bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// test: envelope format — type/severity/uptime_ms + fill fields present
// ---------------------------------------------------------------------------

void test_bb_alert_emit_envelope_format(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_alert_register());
    bb_event_topic_t topic = bb_alert_topic_for_test();

    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_subscribe(topic, capture_alert, NULL, &sub));

    bb_alert_set_min_severity_for_test(BB_ALERT_INFO);
    bb_alert_emit("my_event", BB_ALERT_WARNING, fill_foo_bar, NULL);
    bb_event_pump(0);

    TEST_ASSERT_EQUAL(1, s_event_count);
    TEST_ASSERT_GREATER_THAN(0, (int)s_last_payload_size);

    // Parse and verify envelope fields.
    bb_json_t doc = bb_json_parse(s_last_payload, s_last_payload_size);
    TEST_ASSERT_NOT_NULL(doc);

    char type_buf[64];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(doc, "type", type_buf, sizeof(type_buf)));
    TEST_ASSERT_EQUAL_STRING("my_event", type_buf);

    double sev_val = -1.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(doc, "severity", &sev_val));
    TEST_ASSERT_EQUAL(BB_ALERT_WARNING, (int)sev_val);

    double uptime_val = -1.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(doc, "uptime_ms", &uptime_val));
    TEST_ASSERT_GREATER_OR_EQUAL(0, (int)uptime_val);

    // Fill field
    char foo_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(doc, "foo", foo_buf, sizeof(foo_buf)));
    TEST_ASSERT_EQUAL_STRING("bar", foo_buf);

    bb_json_free(doc);
    bb_event_unsubscribe(sub);
}

// ---------------------------------------------------------------------------
// test: WARNING threshold — WARNING emitted, INFO dropped
// ---------------------------------------------------------------------------

void test_bb_alert_severity_filter_warning_threshold(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_alert_register());
    bb_event_topic_t topic = bb_alert_topic_for_test();

    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_ring_attach_ex(topic, 4, 512, false, &ring));

    bb_alert_set_min_severity_for_test(BB_ALERT_WARNING);

    bb_alert_emit("should_drop", BB_ALERT_INFO, NULL, NULL);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(0, (int)bb_event_ring_count(ring));

    bb_alert_emit("should_emit", BB_ALERT_WARNING, NULL, NULL);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(1, (int)bb_event_ring_count(ring));

    bb_alert_emit("should_also_emit", BB_ALERT_CRITICAL, NULL, NULL);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(2, (int)bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// test: OOM in alert_serialize (bb_json_obj_new fails) — emit must not crash
// and must not post any event to the ring
// ---------------------------------------------------------------------------

void test_bb_alert_emit_serialize_oom(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_alert_register());
    bb_event_topic_t topic = bb_alert_topic_for_test();
    TEST_ASSERT_NOT_NULL(topic);

    bb_event_ring_t ring = NULL;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_event_ring_attach_ex(topic, 4, 512, false, &ring));

    // Fail the very first bb_json_obj_new call inside alert_serialize.
    bb_json_host_force_alloc_fail_after(0);
    bb_alert_emit("oom_event", BB_ALERT_WARNING, NULL, NULL);
    bb_json_host_force_alloc_fail_after(-1);

    bb_event_pump(0);

    // OOM path must silently return — no event posted.
    TEST_ASSERT_EQUAL(0, (int)bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}
