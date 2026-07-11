// Tests for bb_sink_mqtt: MQTT sink adapter wiring.
#include "unity.h"
#include "bb_pub.h"
#include "bb_sink_mqtt.h"
#include "bb_mqtt_client.h"
#include "test_hostname_seed.h"
#include "bb_transport_health.h"

#include <string.h>

// bb_mqtt_client_default_set is exposed by the host backend under BB_MQTT_CLIENT_TESTING.
// bb_mqtt_host_* spy helpers are also gated on BB_MQTT_CLIENT_TESTING (defined in
// the test build via build_flags).
extern void bb_mqtt_client_default_set(bb_mqtt_client_t h);

// ---------------------------------------------------------------------------
// Sample function for tests
// ---------------------------------------------------------------------------

static bool sample_metrics(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "val", 42.0);
    return true;
}

static bool sample_skip(bb_json_t obj, void *ctx)
{
    (void)obj;
    (void)ctx;
    return false;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_sink_mqtt_null_handle_returns_invalid_arg(void)
{
    bb_pub_sink_t s;
    bb_err_t err = bb_sink_mqtt(NULL, &s);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_sink_mqtt_null_out_returns_invalid_arg(void)
{
    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_init(&cfg, &h);
    bb_err_t err = bb_sink_mqtt(h, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    bb_mqtt_client_destroy(h);
}

void test_bb_sink_mqtt_returns_ok(void)
{
    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt(h, &s));
    bb_mqtt_client_destroy(h);
}

void test_bb_sink_mqtt_tick_forwards_to_mqtt_stub(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("cpu", sample_metrics, NULL);

    bb_pub_tick_once();

    // The mqtt stub should have captured exactly one publish
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_pub_count(h));

    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);

    // Topic must be "metrics/mqtthost/cpu"
    TEST_ASSERT_EQUAL_STRING("metrics/mqtthost/cpu", p->topic);

    // Payload must contain the source field and uptime_ms
    TEST_ASSERT_NOT_NULL(strstr(p->payload, "\"val\""));
    TEST_ASSERT_NOT_NULL(strstr(p->payload, "\"uptime_ms\""));

    // QoS and retain must use defaults (0, false)
    TEST_ASSERT_EQUAL_INT(0, p->qos);
    TEST_ASSERT_FALSE(p->retain);

    bb_mqtt_client_destroy(h);
}

void test_bb_sink_mqtt_skipped_source_not_forwarded(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("skip", sample_skip, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_host_pub_count(h));

    bb_mqtt_client_destroy(h);
}

void test_bb_sink_mqtt_multiple_sources_each_forwarded(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("a", sample_metrics, NULL);
    bb_pub_register_source("b", sample_metrics, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, bb_mqtt_client_host_pub_count(h));

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// bb_sink_mqtt_default tests (B1-296: dynamic-handle sink, OTA suspend/resume)
// ---------------------------------------------------------------------------

void test_bb_sink_mqtt_default_null_out_returns_invalid_arg(void)
{
    bb_err_t err = bb_sink_mqtt_default(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_sink_mqtt_default_returns_ok(void)
{
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));
}

void test_bb_sink_mqtt_default_routes_to_live_handle(void)
{
    // Register dynamic sink with a valid default handle; publish must reach it.
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);
    bb_mqtt_client_default_set(h);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("dyn", sample_metrics, NULL);

    bb_pub_tick_once();

    // Publish must have reached the default handle.
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_pub_count(h));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(strstr(p->topic, "/dyn"));

    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

void test_bb_sink_mqtt_default_suspend_is_safe_noop(void)
{
    // Simulate OTA suspend: set default to NULL (handle destroyed/freed).
    // publish must return an error but must NOT crash or touch the old handle.
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);
    bb_mqtt_client_default_set(h);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));

    // Simulate suspend: destroy handle and NULL the default.
    bb_mqtt_client_destroy(h);
    h = NULL;
    bb_mqtt_client_default_set(NULL);

    // Direct publish call during suspend window must be a safe no-op.
    bb_err_t rc = s.publish(s.ctx, "metrics/mqtthost/test", "{\"val\":1}", 9, false);
    // bb_mqtt_client_publish(NULL, ...) returns BB_ERR_INVALID_ARG — non-zero, no crash.
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// bb_transport_health register + report wiring (B1-518 PR2, OBSERVE-ONLY)
// ---------------------------------------------------------------------------

void test_bb_sink_mqtt_publish_success_reports_transport_health(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");
    bb_transport_health_reset_for_test();
    bb_sink_mqtt_reset_transport_health_for_test();

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_err_t rc = s.publish(s.ctx, "metrics/mqtthost/x", "{}", 2, false);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(1, enabled);
    TEST_ASSERT_EQUAL(0, failing);

    bb_mqtt_client_destroy(h);
}

void test_bb_sink_mqtt_default_publish_failure_reports_transport_health(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");
    bb_transport_health_reset_for_test();
    bb_sink_mqtt_reset_transport_health_for_test();

    // No default handle set — bb_mqtt_client_default() returns NULL, so
    // bb_mqtt_client_publish(NULL, ...) fails with BB_ERR_INVALID_ARG.
    bb_mqtt_client_default_set(NULL);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));
    bb_err_t rc = s.publish(s.ctx, "metrics/mqtthost/x", "{}", 2, false);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(1, enabled);
    TEST_ASSERT_EQUAL(1, failing);
}

void test_bb_sink_mqtt_default_resume_routes_to_new_handle(void)
{
    // After resume, the dynamic sink must route to the NEW handle, never the old.
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");

    // Boot: set old handle as default.
    bb_mqtt_client_t old_h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &old_h));
    bb_mqtt_client_host_reset(old_h);
    bb_mqtt_client_default_set(old_h);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("dyn2", sample_metrics, NULL);

    // Simulate suspend: destroy old handle, NULL default.
    bb_mqtt_client_destroy(old_h);
    old_h = NULL;
    bb_mqtt_client_default_set(NULL);

    // Simulate resume: create NEW handle at different address.
    bb_mqtt_client_t new_h;
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &new_h));
    bb_mqtt_client_host_reset(new_h);
    bb_mqtt_client_default_set(new_h);

    // Tick: publish must reach the NEW handle, not the old (freed) one.
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_client_host_pub_count(new_h));
    const bb_mqtt_client_host_pub_t *p = bb_mqtt_client_host_last_pub(new_h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(strstr(p->topic, "/dyn2"));

    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(new_h);
}

// Lazy register-once: publish() N times must register exactly once, not
// once per call (enabled count stays 1 across repeated publishes).
void test_bb_sink_mqtt_publish_registers_transport_health_once(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");
    bb_transport_health_reset_for_test();
    bb_sink_mqtt_reset_transport_health_for_test();

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);

    bb_err_t rc = s.publish(s.ctx, "metrics/mqtthost/x", "{}", 2, false);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(1, enabled);

    rc = s.publish(s.ctx, "metrics/mqtthost/y", "{}", 2, false);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    enabled = -1;
    failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL(1, enabled);

    bb_mqtt_client_destroy(h);
}

// Slot-exhaustion degrade: when bb_transport_health has no free slots, the
// lazy register-on-publish call fails, but the sink's own publish() outcome
// must still reflect the real transport result — never BB_ERR_NO_SPACE.
void test_bb_sink_mqtt_publish_survives_transport_health_slot_exhaustion(void)
{
    bb_pub_test_reset();
    bb_test_seed_hostname("mqtthost");
    bb_transport_health_reset_for_test();
    bb_sink_mqtt_reset_transport_health_for_test();

    // Fill every slot directly so the sink's lazy register() call fails.
    bb_transport_handle_t th;
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_register("filler", BB_TRANSPORT_AUTHORITATIVE, &th));
    }

    bb_mqtt_client_t h;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);

    // Success path: table is full, but the real transport outcome (BB_OK) wins.
    bb_err_t rc = s.publish(s.ctx, "metrics/mqtthost/x", "{}", 2, false);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(BB_ERR_NO_SPACE, rc);

    bb_mqtt_client_destroy(h);

    // Failure path: real transport error wins, not BB_ERR_NO_SPACE.
    // bb_sink_mqtt_default() with no default handle set fails at bb_mqtt_client_publish
    // with BB_ERR_INVALID_ARG (mirrors test_bb_sink_mqtt_default_publish_failure_reports_transport_health).
    bb_mqtt_client_default_set(NULL);
    bb_pub_sink_t sdef;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&sdef));
    rc = sdef.publish(sdef.ctx, "metrics/mqtthost/y", "{}", 2, false);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(BB_ERR_NO_SPACE, rc);
}

