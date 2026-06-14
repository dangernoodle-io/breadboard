// Tests for bb_pub_info: sample fn always publishes device info fields.
#include "unity.h"
#include "bb_pub_info.h"
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
} capture_entry_t;

static capture_entry_t s_captured[CAPTURE_CAP];
static int             s_capture_count;

static bb_err_t capture_publish(void *ctx, const char *topic,
                                 const char *payload, int len)
{
    (void)ctx;
    (void)len;
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

static void setup(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_info_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_info_always_publishes(void)
{
    setup();
    bb_pub_tick_once();
    // Info source always returns true — must publish even with no hardware HALs.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_info_topic_is_correct(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/info", s_captured[0].topic);
}

void test_bb_pub_info_has_heap_internal_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_free\""));
}

void test_bb_pub_info_has_heap_internal_total(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_total\""));
}

void test_bb_pub_info_has_psram_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"psram_free\""));
}

void test_bb_pub_info_has_psram_total(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"psram_total\""));
}

void test_bb_pub_info_has_uptime_ms(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"uptime_ms\""));
}

void test_bb_pub_info_has_version(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"version\""));
}

void test_bb_pub_info_has_heap_internal_largest_block(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_largest_block\""));
}

void test_bb_pub_info_has_heap_internal_min_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_min_free\""));
}

void test_bb_pub_info_has_rtc_used(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rtc_used\""));
}

void test_bb_pub_info_has_rtc_total(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rtc_total\""));
}

void test_bb_pub_info_has_flash_size(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"flash_size\""));
}

void test_bb_pub_info_has_app_size(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"app_size\""));
}

void test_bb_pub_info_has_wdt_resets(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"wdt_resets\""));
}

void test_bb_pub_info_payload_has_ts_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}

void test_bb_pub_info_has_reset_reason(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"reset_reason\""));
}

void test_bb_pub_info_reset_reason_is_power_on_on_host(void)
{
    setup();
    bb_pub_tick_once();
    // Host stub for bb_board_get_reset_reason returns "power-on".
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"reset_reason\":\"power-on\""));
}

void test_bb_pub_info_has_ota_validated(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ota_validated\""));
}

void test_bb_pub_info_ota_validated_is_false_on_host(void)
{
    setup();
    bb_pub_tick_once();
    // Host stub for bb_ota_is_validated returns false.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ota_validated\":false"));
}

void test_bb_pub_info_has_rtc_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rtc_free\""));
}

void test_bb_pub_info_rtc_free_is_zero_on_host(void)
{
    setup();
    bb_pub_tick_once();
    // Host stubs return rtc_total=0 and rtc_used=0, so rtc_free=0.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rtc_free\":0"));
}

void test_bb_pub_info_has_time_valid(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"time_valid\""));
}

void test_bb_pub_info_time_valid_is_false_on_host(void)
{
    setup();
    bb_pub_tick_once();
    // Host stub for bb_ntp_is_synced returns false → time_valid=false.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"time_valid\":false"));
}

void test_bb_pub_info_has_epoch_s(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"epoch_s\""));
}

void test_bb_pub_info_epoch_s_is_zero_when_not_synced(void)
{
    setup();
    bb_pub_tick_once();
    // bb_ntp_is_synced() returns false on host → epoch_s=0.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"epoch_s\":0"));
}

void test_bb_pub_info_has_time_source(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"time_source\""));
}

void test_bb_pub_info_time_source_is_none_on_host(void)
{
    setup();
    bb_pub_tick_once();
    // bb_ntp_is_synced() returns false on host → time_source="none".
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"time_source\":\"none\""));
}
