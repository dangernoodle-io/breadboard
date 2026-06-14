// Tests for bb_sink_mqtt: MQTT sink adapter wiring.
#include "unity.h"
#include "bb_pub.h"
#include "bb_sink_mqtt.h"
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <string.h>

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

