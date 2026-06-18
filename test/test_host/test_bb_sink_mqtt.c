// Tests for bb_sink_mqtt: MQTT sink adapter wiring.
#include "unity.h"
#include "bb_pub.h"
#include "bb_sink_mqtt.h"
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <string.h>

// bb_mqtt_default_set is exposed by the host backend under BB_MQTT_TESTING.
// bb_mqtt_host_* spy helpers are also gated on BB_MQTT_TESTING (defined in
// the test build via build_flags).
extern void bb_mqtt_default_set(bb_mqtt_t h);

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
    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_init(&cfg, &h);
    bb_err_t err = bb_sink_mqtt(h, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    bb_mqtt_destroy(h);
}

void test_bb_sink_mqtt_returns_ok(void)
{
    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &h));
    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt(h, &s));
    bb_mqtt_destroy(h);
}

void test_bb_sink_mqtt_tick_forwards_to_mqtt_stub(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("mqtthost");

    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &h));
    bb_mqtt_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("cpu", sample_metrics, NULL);

    bb_pub_tick_once();

    // The mqtt stub should have captured exactly one publish
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_host_pub_count(h));

    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);

    // Topic must be "metrics/mqtthost/cpu"
    TEST_ASSERT_EQUAL_STRING("metrics/mqtthost/cpu", p->topic);

    // Payload must contain the source field and ts
    TEST_ASSERT_NOT_NULL(strstr(p->payload, "\"val\""));
    TEST_ASSERT_NOT_NULL(strstr(p->payload, "\"ts\""));

    // QoS and retain must use defaults (0, false)
    TEST_ASSERT_EQUAL_INT(0, p->qos);
    TEST_ASSERT_FALSE(p->retain);

    bb_mqtt_destroy(h);
}

void test_bb_sink_mqtt_skipped_source_not_forwarded(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("mqtthost");

    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &h));
    bb_mqtt_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("skip", sample_skip, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_host_pub_count(h));

    bb_mqtt_destroy(h);
}

void test_bb_sink_mqtt_multiple_sources_each_forwarded(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("mqtthost");

    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &h));
    bb_mqtt_host_reset(h);

    bb_pub_sink_t s;
    bb_sink_mqtt(h, &s);
    bb_pub_set_sink(&s);
    bb_pub_register_source("a", sample_metrics, NULL);
    bb_pub_register_source("b", sample_metrics, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, bb_mqtt_host_pub_count(h));

    bb_mqtt_destroy(h);
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
    bb_nv_config_set_hostname("mqtthost");

    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &h));
    bb_mqtt_host_reset(h);
    bb_mqtt_default_set(h);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("dyn", sample_metrics, NULL);

    bb_pub_tick_once();

    // Publish must have reached the default handle.
    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_host_pub_count(h));
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(strstr(p->topic, "/dyn"));

    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(h);
}

void test_bb_sink_mqtt_default_suspend_is_safe_noop(void)
{
    // Simulate OTA suspend: set default to NULL (handle destroyed/freed).
    // publish must return an error but must NOT crash or touch the old handle.
    bb_pub_test_reset();
    bb_nv_config_set_hostname("mqtthost");

    bb_mqtt_t h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &h));
    bb_mqtt_host_reset(h);
    bb_mqtt_default_set(h);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));

    // Simulate suspend: destroy handle and NULL the default.
    bb_mqtt_destroy(h);
    h = NULL;
    bb_mqtt_default_set(NULL);

    // Direct publish call during suspend window must be a safe no-op.
    bb_err_t rc = s.publish(s.ctx, "metrics/mqtthost/test", "{\"val\":1}", 9);
    // bb_mqtt_publish(NULL, ...) returns BB_ERR_INVALID_ARG — non-zero, no crash.
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
}

void test_bb_sink_mqtt_default_resume_routes_to_new_handle(void)
{
    // After resume, the dynamic sink must route to the NEW handle, never the old.
    bb_pub_test_reset();
    bb_nv_config_set_hostname("mqtthost");

    // Boot: set old handle as default.
    bb_mqtt_t old_h;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &old_h));
    bb_mqtt_host_reset(old_h);
    bb_mqtt_default_set(old_h);

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_mqtt_default(&s));
    bb_pub_set_sink(&s);
    bb_pub_register_source("dyn2", sample_metrics, NULL);

    // Simulate suspend: destroy old handle, NULL default.
    bb_mqtt_destroy(old_h);
    old_h = NULL;
    bb_mqtt_default_set(NULL);

    // Simulate resume: create NEW handle at different address.
    bb_mqtt_t new_h;
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_init(&cfg, &new_h));
    bb_mqtt_host_reset(new_h);
    bb_mqtt_default_set(new_h);

    // Tick: publish must reach the NEW handle, not the old (freed) one.
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, bb_mqtt_host_pub_count(new_h));
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(new_h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(strstr(p->topic, "/dyn2"));

    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(new_h);
}

