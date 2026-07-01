// Tests for bb_pub_info: sample fn always publishes device info fields.
#include "unity.h"
#include "bb_pub_info.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "../../platform/host/bb_board/bb_board_test.h"

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

static void setup(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_board_test_set_ota_validated(false);   /* reset board hook to default */
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

void test_bb_pub_info_omits_psram_fields_when_no_psram(void)
{
    setup();
    // Host stub returns psram_total==0 (no PSRAM hardware).
    // Both psram_free and psram_total must be absent from the payload.
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"psram_free\""));
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"psram_total\""));
}

void test_bb_pub_info_has_uptime_ms(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts_ms\""));
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

// TA-505: rtc_used, rtc_total, dram_static_bytes, flash_size, app_size are
// static identity fields moved to the meta topic — must be ABSENT from info.

void test_bb_pub_info_does_not_emit_rtc_used(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"rtc_used\""));
}

void test_bb_pub_info_does_not_emit_rtc_total(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"rtc_total\""));
}

void test_bb_pub_info_does_not_emit_dram_static_bytes(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"dram_static_bytes\""));
}

void test_bb_pub_info_does_not_emit_flash_size(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"flash_size\""));
}

void test_bb_pub_info_does_not_emit_app_size(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"app_size\""));
}

void test_bb_pub_info_has_wdt_resets(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"wdt_resets\""));
}

void test_bb_pub_info_payload_has_uptime_ms_field(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts_ms\""));
}

// TA-505: reset_reason is a static identity field moved to the meta topic.
void test_bb_pub_info_does_not_emit_reset_reason(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"reset_reason\""));
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
    bb_board_test_set_ota_validated(false);
    bb_pub_tick_once();
    // bb_board_get_info().ota_validated returns false (host stub default).
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ota_validated\":false"));
}

// B1-299: bb_pub_info must agree with bb_board_get_info().ota_validated after
// a simulated mark-valid (set via bb_board_test_set_ota_validated).
void test_bb_pub_info_ota_validated_agrees_with_bb_board_after_mark_valid(void)
{
    setup();

    // Simulate a successful post-boot mark-valid: board now reports validated.
    bb_board_test_set_ota_validated(true);

    bb_pub_tick_once();

    // bb_pub_info must now report true — same as bb_board_get_info().ota_validated.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ota_validated\":true"));
}

// TA-505: rtc_free was a derived field (rtc_total - rtc_used); it has been
// dropped entirely — neither info nor meta emit it.
void test_bb_pub_info_does_not_emit_rtc_free(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"rtc_free\""));
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

// TA-505: boot_epoch_s, time_source, board, chip_model, mac are static identity
// fields moved to the meta topic — must be ABSENT from info.

void test_bb_pub_info_does_not_emit_boot_epoch_s(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"boot_epoch_s\""));
}

void test_bb_pub_info_does_not_emit_time_source(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"time_source\""));
}

void test_bb_pub_info_does_not_emit_board(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"board\""));
}

void test_bb_pub_info_does_not_emit_chip_model(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"chip_model\""));
}

void test_bb_pub_info_does_not_emit_mac(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"mac\""));
}

void test_bb_pub_info_does_not_emit_version(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"version\""));
}

// B1-heap-internal-min-fix: heap_internal_min_free must use MALLOC_CAP_INTERNAL,
// not MALLOC_CAP_DEFAULT. On host both stubs return 0; verify the field is
// present and numeric (not absent).
void test_bb_pub_info_heap_internal_min_free_present(void)
{
    setup();
    bb_pub_tick_once();
    // Field must be present and carry a numeric value (0 on host stubs).
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"heap_internal_min_free\":0"));
}

// ---------------------------------------------------------------------------
// bb_mem accounting fields (B1-heap-trace)
// ---------------------------------------------------------------------------

void test_bb_pub_info_has_bb_mem_out(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"bb_mem_out\""));
}

void test_bb_pub_info_has_bb_mem_peak(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"bb_mem_peak\""));
}

void test_bb_pub_info_has_bb_mem_fail(void)
{
    setup();
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"bb_mem_fail\""));
}
