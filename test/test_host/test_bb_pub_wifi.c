// Tests for bb_pub_wifi: full wifi connection telemetry source.
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

// Helper: populate a full test info struct.
static bb_pub_wifi_test_info_t make_info(bool connected, int8_t rssi)
{
    bb_pub_wifi_test_info_t info;
    memset(&info, 0, sizeof(info));
    info.connected   = connected;
    info.rssi        = rssi;
    strncpy(info.ssid, "TestNet", sizeof(info.ssid) - 1);
    info.bssid[0] = 0xAA; info.bssid[1] = 0xBB; info.bssid[2] = 0xCC;
    info.bssid[3] = 0xDD; info.bssid[4] = 0xEE; info.bssid[5] = 0xFF;
    strncpy(info.ip, "192.168.1.42", sizeof(info.ip) - 1);
    info.disc_reason = 3;
    info.disc_age_s  = 120;
    info.retry_count = 2;
    return info;
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

// Telem (cache) path stamps a sample-time ts_ms into the snapshot and emits it
// from the serializer (replacing the legacy post-injected uptime_ms) so that
// REST == SSE == sink bytes are identical. (Name kept for history continuity;
// the timestamp field is now ts_ms, sourced from the snapshot.)
void test_bb_pub_wifi_payload_has_uptime_ms_field(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -60);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts_ms\""));
    // Legacy post-injected uptime_ms must NOT appear on the telem path.
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"uptime_ms\""));
}

// ---------------------------------------------------------------------------
// New parity tests: full wifi info fields
// ---------------------------------------------------------------------------

void test_bb_pub_wifi_has_ssid_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ssid\""));
}

void test_bb_pub_wifi_ssid_value_correct(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "TestNet"));
}

void test_bb_pub_wifi_has_bssid_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"bssid\""));
}

void test_bb_pub_wifi_bssid_format_correct(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // bssid should be "aa:bb:cc:dd:ee:ff"
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "aa:bb:cc:dd:ee:ff"));
}

void test_bb_pub_wifi_has_ip_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ip\""));
}

void test_bb_pub_wifi_ip_value_correct(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "192.168.1.42"));
}

void test_bb_pub_wifi_has_connected_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"connected\""));
}

void test_bb_pub_wifi_has_disc_reason_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"disc_reason\""));
}

void test_bb_pub_wifi_has_disc_age_s_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"disc_age_s\""));
}

void test_bb_pub_wifi_has_retry_count_field(void)
{
    setup();
    bb_pub_wifi_test_info_t info = make_info(true, -65);
    bb_pub_wifi_test_set_info(&info);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"retry_count\""));
}

void test_bb_pub_wifi_has_no_ip_recoveries_field(void)
{
    setup();
    bb_pub_wifi_test_set_rssi(true, -65);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"no_ip_recoveries\""));
}

void test_bb_pub_wifi_rssi_is_integer_not_float(void)
{
    // After the integer-setter fix, rssi must NOT appear as a decimal (e.g. -55.0).
    // cJSON serializes small whole doubles without a decimal point so "-55" is fine;
    // "-55.0" or "-55.000" would indicate set_number(double) was still used.
    setup();
    bb_pub_wifi_test_set_rssi(true, -55);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL_MESSAGE(strstr(s_captured[0].payload, "-55."),
                             "rssi should be integer (no decimal point)");
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "-55"));
}
