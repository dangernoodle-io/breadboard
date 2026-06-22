// Tests for bb_pub_health: operational health telemetry source.
#include "unity.h"
#include "bb_pub_health.h"
#include "bb_pub.h"
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fake capturing sink
// ---------------------------------------------------------------------------

#define CAPTURE_CAP 16

typedef struct {
    char topic[192];
    char payload[512];
} health_capture_entry_t;

static health_capture_entry_t s_captured[CAPTURE_CAP];
static int                    s_capture_count;

static bb_err_t capture_publish(void *ctx, const char *topic,
                                 const char *payload, int len)
{
    (void)ctx;
    (void)len;
    if (s_capture_count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    health_capture_entry_t *e = &s_captured[s_capture_count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

static void capture_reset(void)
{
    memset(s_captured, 0, sizeof(s_captured));
    s_capture_count = 0;
}

static void setup(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
    bb_mqtt_default_set(NULL);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_health_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_health_always_publishes(void)
{
    setup();
    bb_pub_tick_once();
    // Health source always returns true.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_health_topic_is_correct(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/health", s_captured[0].topic);
}

void test_bb_pub_health_has_ok_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ok\""));
}

void test_bb_pub_health_ok_is_false_on_host(void)
{
    setup();
    bb_pub_tick_once();
    // Host: bb_wifi_has_ip() = false → ok = false
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ok\":false"));
}

void test_bb_pub_health_has_mqtt_enabled_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_enabled\""));
}

void test_bb_pub_health_mqtt_enabled_false_when_no_handle(void)
{
    setup();
    // bb_mqtt_default() returns NULL by default on host
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_enabled\":false"));
}

void test_bb_pub_health_has_mqtt_connected_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_connected\""));
}

void test_bb_pub_health_mqtt_connected_false_when_no_handle(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_connected\":false"));
}

void test_bb_pub_health_mqtt_enabled_true_when_handle_set(void)
{
    setup();
    bb_mqtt_t h = NULL;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_init(&cfg, &h));
    bb_mqtt_default_set(h);

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_enabled\":true"));

    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(h);
}

void test_bb_pub_health_has_mqtt_reconnect_count_when_enabled(void)
{
    setup();
    bb_mqtt_t h = NULL;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_init(&cfg, &h));
    bb_mqtt_default_set(h);

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_reconnect_count\""));

    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(h);
}

void test_bb_pub_health_mqtt_connected_reflects_state(void)
{
    setup();
    bb_mqtt_t h = NULL;
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_init(&cfg, &h));
    bb_mqtt_default_set(h);
    bb_mqtt_host_set_connected(h, false);

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mqtt_connected\":false"));

    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(h);
}

void test_bb_pub_health_payload_has_ts_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}
