// Tests for bb_pub_wifi: wifi RSSI telemetry source.
#include "unity.h"
#include "bb_pub_wifi.h"
#include "bb_pub.h"
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
} wifi_capture_entry_t;

static wifi_capture_entry_t s_captured[CAPTURE_CAP];
static int                  s_capture_count;

static bb_err_t capture_publish(void *ctx, const char *topic,
                                const char *payload, int len)
{
    (void)ctx;
    (void)len;
    if (s_capture_count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    wifi_capture_entry_t *e = &s_captured[s_capture_count++];
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

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_wifi_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_wifi_skips_when_disconnected(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(false, 0);
    bb_pub_tick_once();
    // Source returns false → nothing published.
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_wifi_publishes_when_connected(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -65);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_wifi_topic_is_correct(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -70);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/wifi", s_captured[0].topic);
}

void test_bb_pub_wifi_has_rssi_field(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -72);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rssi\""));
}

void test_bb_pub_wifi_rssi_value_correct(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -55);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "-55"));
}

void test_bb_pub_wifi_payload_has_ts_field(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -60);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}
