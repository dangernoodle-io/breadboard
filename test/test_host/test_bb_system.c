#include "unity.h"
#include "bb_system.h"
#include "bb_system_test.h"
#include <string.h>

void test_bb_system_get_version_returns_nonnull(void)
{
    const char *v = bb_system_get_version();
    TEST_ASSERT_NOT_NULL(v);
}

void test_bb_system_get_version_default_is_host_string(void)
{
    const char *v = bb_system_get_version();
    TEST_ASSERT_EQUAL_STRING("0.0.0-host", v);
}

void test_bb_system_get_project_name_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_project_name();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_system_get_build_date_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_build_date();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_system_get_build_time_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_build_time();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_system_get_idf_version_returns_nonnull_nonempty(void)
{
    const char *v = bb_system_get_idf_version();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_bb_error_check_happy_path(void)
{
    // BB_ERROR_CHECK with BB_OK must not abort
    BB_ERROR_CHECK(BB_OK);
    TEST_PASS();
}

void test_bb_system_read_temp_default_unsupported(void)
{
    // host default: no sensor, must return BB_ERR_UNSUPPORTED
    bb_system_set_temp_for_test(0.0f, BB_ERR_UNSUPPORTED);
    float f = -999.0f;
    bb_err_t rc = bb_system_read_temp_celsius(&f);
    TEST_ASSERT_EQUAL_INT(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_FLOAT(-999.0f, f);  // *out untouched on error
}

void test_bb_system_read_temp_null_out(void)
{
    bb_err_t rc = bb_system_read_temp_celsius(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_system_read_temp_injected_ok(void)
{
    bb_system_set_temp_for_test(42.5f, BB_OK);
    float f = 0.0f;
    bb_err_t rc = bb_system_read_temp_celsius(&f);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_FLOAT(42.5f, f);
}

void test_bb_system_read_temp_injected_error(void)
{
    bb_system_set_temp_for_test(0.0f, BB_ERR_UNSUPPORTED);
    float f = -1.0f;
    bb_err_t rc = bb_system_read_temp_celsius(&f);
    TEST_ASSERT_EQUAL_INT(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, f);  // *out untouched on error
}

// Byte-identical value guard (B1-463): bb_system_reset_reason_str is driven by
// the shared BB_RESET_REASON_LIST X-macro across espidf/host/arduino. These
// assertions pin the exact wire strings so the X-macro can never silently drift.
void test_bb_system_reset_reason_str_poweron(void)
{
    TEST_ASSERT_EQUAL_STRING("power-on", bb_system_reset_reason_str(BB_RESET_REASON_POWERON));
}

void test_bb_system_reset_reason_str_ext(void)
{
    TEST_ASSERT_EQUAL_STRING("ext", bb_system_reset_reason_str(BB_RESET_REASON_EXT));
}

void test_bb_system_reset_reason_str_sw(void)
{
    TEST_ASSERT_EQUAL_STRING("software", bb_system_reset_reason_str(BB_RESET_REASON_SW));
}

void test_bb_system_reset_reason_str_panic(void)
{
    TEST_ASSERT_EQUAL_STRING("panic", bb_system_reset_reason_str(BB_RESET_REASON_PANIC));
}

void test_bb_system_reset_reason_str_int_wdt(void)
{
    TEST_ASSERT_EQUAL_STRING("int_wdt", bb_system_reset_reason_str(BB_RESET_REASON_INT_WDT));
}

void test_bb_system_reset_reason_str_task_wdt(void)
{
    TEST_ASSERT_EQUAL_STRING("task_wdt", bb_system_reset_reason_str(BB_RESET_REASON_TASK_WDT));
}

void test_bb_system_reset_reason_str_wdt(void)
{
    TEST_ASSERT_EQUAL_STRING("wdt", bb_system_reset_reason_str(BB_RESET_REASON_WDT));
}

void test_bb_system_reset_reason_str_deepsleep(void)
{
    TEST_ASSERT_EQUAL_STRING("deep_sleep", bb_system_reset_reason_str(BB_RESET_REASON_DEEPSLEEP));
}

void test_bb_system_reset_reason_str_brownout(void)
{
    TEST_ASSERT_EQUAL_STRING("brownout", bb_system_reset_reason_str(BB_RESET_REASON_BROWNOUT));
}

void test_bb_system_reset_reason_str_sdio(void)
{
    TEST_ASSERT_EQUAL_STRING("sdio", bb_system_reset_reason_str(BB_RESET_REASON_SDIO));
}

void test_bb_system_reset_reason_str_unknown(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_system_reset_reason_str(BB_RESET_REASON_UNKNOWN));
}

void test_bb_system_reset_reason_str_out_of_range(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_system_reset_reason_str((bb_reset_reason_t)999));
}

// ---------------------------------------------------------------------------
// bb_system_get_app_sha256 (B1-893: lost its only production caller,
// bb_info_build.c, when bb_info was deleted; direct tests keep this
// still-public bb_system API surface covered).
// ---------------------------------------------------------------------------

void test_bb_system_get_app_sha256_null_out_returns_invalid_arg(void)
{
    char buf[16];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_system_get_app_sha256(NULL, sizeof(buf)));
}

void test_bb_system_get_app_sha256_too_small_returns_no_space(void)
{
    char buf[9];
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_system_get_app_sha256(buf, sizeof(buf)));
}

void test_bb_system_get_app_sha256_writes_fixed_test_value(void)
{
    char buf[16] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_system_get_app_sha256(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("deadbeef0", buf);
}

// ---------------------------------------------------------------------------
// bb_reset_source_str — byte-identical wire-string guard (mirrors the
// reset_reason pinning above). BB_RESET_SRC_LIST drives the switch.
// ---------------------------------------------------------------------------

void test_bb_reset_source_str_api_reboot(void)
{
    TEST_ASSERT_EQUAL_STRING("api_reboot", bb_reset_source_str(BB_RESET_SRC_API_REBOOT));
}

void test_bb_reset_source_str_factory_reset(void)
{
    TEST_ASSERT_EQUAL_STRING("factory_reset", bb_reset_source_str(BB_RESET_SRC_FACTORY_RESET));
}

void test_bb_reset_source_str_wifi_safeguard(void)
{
    TEST_ASSERT_EQUAL_STRING("wifi_safeguard", bb_reset_source_str(BB_RESET_SRC_WIFI_SAFEGUARD));
}

void test_bb_reset_source_str_wifi_cold_timeout(void)
{
    TEST_ASSERT_EQUAL_STRING("wifi_cold_timeout", bb_reset_source_str(BB_RESET_SRC_WIFI_COLD_TIMEOUT));
}

void test_bb_reset_source_str_wifi_pending_revert(void)
{
    TEST_ASSERT_EQUAL_STRING("wifi_pending_revert", bb_reset_source_str(BB_RESET_SRC_WIFI_PENDING_REVERT));
}

void test_bb_reset_source_str_wifi_reconfigure(void)
{
    TEST_ASSERT_EQUAL_STRING("wifi_reconfigure", bb_reset_source_str(BB_RESET_SRC_WIFI_RECONFIGURE));
}

void test_bb_reset_source_str_egress_tier3(void)
{
    TEST_ASSERT_EQUAL_STRING("egress_tier3", bb_reset_source_str(BB_RESET_SRC_EGRESS_TIER3));
}

void test_bb_reset_source_str_ota_pull_applied(void)
{
    TEST_ASSERT_EQUAL_STRING("ota_pull_applied", bb_reset_source_str(BB_RESET_SRC_OTA_PULL_APPLIED));
}

void test_bb_reset_source_str_ota_push_applied(void)
{
    TEST_ASSERT_EQUAL_STRING("ota_push_applied", bb_reset_source_str(BB_RESET_SRC_OTA_PUSH_APPLIED));
}

void test_bb_reset_source_str_ota_boot_apply(void)
{
    TEST_ASSERT_EQUAL_STRING("ota_boot_apply", bb_reset_source_str(BB_RESET_SRC_OTA_BOOT_APPLY));
}

void test_bb_reset_source_str_ota_boot_done(void)
{
    TEST_ASSERT_EQUAL_STRING("ota_boot_done", bb_reset_source_str(BB_RESET_SRC_OTA_BOOT_DONE));
}

void test_bb_reset_source_str_ota_boot_abort(void)
{
    TEST_ASSERT_EQUAL_STRING("ota_boot_abort", bb_reset_source_str(BB_RESET_SRC_OTA_BOOT_ABORT));
}

void test_bb_reset_source_str_unknown(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_reset_source_str(BB_RESET_SRC_UNKNOWN));
}

void test_bb_reset_source_str_out_of_range(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_reset_source_str((bb_reset_source_t)999));
}

// ---------------------------------------------------------------------------
// bb_reboot_record_encode / bb_reboot_record_decode — pure pack/unpack.
// ---------------------------------------------------------------------------

void test_bb_reboot_record_encode_null_args(void)
{
    char buf[BB_REBOOT_RECORD_STR_MAX];
    bb_reboot_record_t r = {0};
    TEST_ASSERT_FALSE(bb_reboot_record_encode(NULL, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(bb_reboot_record_encode(&r, NULL, sizeof(buf)));
    TEST_ASSERT_FALSE(bb_reboot_record_encode(&r, buf, 0));
}

void test_bb_reboot_record_encode_decode_roundtrip(void)
{
    bb_reboot_record_t r = {
        .src      = BB_RESET_SRC_EGRESS_TIER3,
        .epoch_s  = 1735689600U,
        .uptime_s = 3600U,
    };
    strcpy(r.detail, "gw unreachable");

    char buf[BB_REBOOT_RECORD_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_record_encode(&r, buf, sizeof(buf)));

    bb_reboot_record_t out = {0};
    TEST_ASSERT_TRUE(bb_reboot_record_decode(buf, &out));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BB_RESET_SRC_EGRESS_TIER3, out.src);
    TEST_ASSERT_EQUAL_UINT32(1735689600U, out.epoch_s);
    TEST_ASSERT_EQUAL_UINT32(3600U, out.uptime_s);
    TEST_ASSERT_EQUAL_STRING("gw unreachable", out.detail);
}

void test_bb_reboot_record_encode_decode_empty_detail(void)
{
    bb_reboot_record_t r = { .src = BB_RESET_SRC_API_REBOOT, .epoch_s = 0, .uptime_s = 0 };
    r.detail[0] = '\0';

    char buf[BB_REBOOT_RECORD_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_record_encode(&r, buf, sizeof(buf)));

    bb_reboot_record_t out;
    memset(&out, 0xAA, sizeof(out));
    TEST_ASSERT_TRUE(bb_reboot_record_decode(buf, &out));
    TEST_ASSERT_EQUAL_STRING("", out.detail);
    TEST_ASSERT_EQUAL_UINT32(0U, out.epoch_s);
}

void test_bb_reboot_record_encode_truncates_detail_at_pipe(void)
{
    bb_reboot_record_t r = { .src = BB_RESET_SRC_OTA_PULL_APPLIED, .epoch_s = 1, .uptime_s = 1 };
    strcpy(r.detail, "abc|def");

    char buf[BB_REBOOT_RECORD_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_record_encode(&r, buf, sizeof(buf)));

    bb_reboot_record_t out;
    TEST_ASSERT_TRUE(bb_reboot_record_decode(buf, &out));
    TEST_ASSERT_EQUAL_STRING("abc", out.detail);
}

void test_bb_reboot_record_encode_truncates_detail_over_48_chars(void)
{
    bb_reboot_record_t r = { .src = BB_RESET_SRC_WIFI_SAFEGUARD, .epoch_s = 1, .uptime_s = 1 };
    memset(r.detail, 'x', sizeof(r.detail) - 1);
    r.detail[sizeof(r.detail) - 1] = '\0';
    // detail is already at capacity (48 chars); this is the boundary case,
    // not an overflow — confirms encode doesn't off-by-one truncate.

    char buf[BB_REBOOT_RECORD_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_record_encode(&r, buf, sizeof(buf)));

    bb_reboot_record_t out;
    TEST_ASSERT_TRUE(bb_reboot_record_decode(buf, &out));
    TEST_ASSERT_EQUAL_INT(48, (int)strlen(out.detail));
}

void test_bb_reboot_record_encode_unterminated_detail(void)
{
    // detail has no NUL within its 49-byte array at all (e.g. a future
    // caller that doesn't NUL-terminate) — encode must bound the scan to
    // sizeof(detail) rather than reading past the field, and still
    // truncate cleanly to 48 chars.
    bb_reboot_record_t r = { .src = BB_RESET_SRC_WIFI_SAFEGUARD, .epoch_s = 1, .uptime_s = 1 };
    memset(r.detail, 'x', sizeof(r.detail));

    char buf[BB_REBOOT_RECORD_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_record_encode(&r, buf, sizeof(buf)));

    bb_reboot_record_t out;
    TEST_ASSERT_TRUE(bb_reboot_record_decode(buf, &out));
    TEST_ASSERT_EQUAL_INT(48, (int)strlen(out.detail));
}

void test_bb_reboot_record_encode_buffer_too_small(void)
{
    bb_reboot_record_t r = { .src = BB_RESET_SRC_OTA_BOOT_DONE, .epoch_s = 1, .uptime_s = 1 };
    strcpy(r.detail, "some detail text");

    char tiny[4];
    TEST_ASSERT_FALSE(bb_reboot_record_encode(&r, tiny, sizeof(tiny)));
}

void test_bb_reboot_record_decode_null_args(void)
{
    bb_reboot_record_t out;
    TEST_ASSERT_FALSE(bb_reboot_record_decode(NULL, &out));
    TEST_ASSERT_FALSE(bb_reboot_record_decode("0|0|0|", NULL));
}

void test_bb_reboot_record_decode_malformed(void)
{
    bb_reboot_record_t out;
    TEST_ASSERT_FALSE(bb_reboot_record_decode("", &out));
    TEST_ASSERT_FALSE(bb_reboot_record_decode("not-a-record", &out));
    TEST_ASSERT_FALSE(bb_reboot_record_decode("1|2", &out));
}

void test_bb_reboot_record_decode_rejects_missing_trailing_pipe(void)
{
    // Three numeric fields with no trailing '|' satisfy the three %u
    // conversions but never reach the %n — consumed stays 0 and decode
    // must reject rather than reading uninitialized/garbage detail.
    bb_reboot_record_t out;
    TEST_ASSERT_FALSE(bb_reboot_record_decode("0|0|0", &out));
}

void test_bb_reboot_record_decode_rejects_leading_sign(void)
{
    // A leading '-' on the src field must be rejected outright rather than
    // silently wrapping into a huge unsigned value via %u's sign lenience.
    bb_reboot_record_t out;
    TEST_ASSERT_FALSE(bb_reboot_record_decode("-1|0|0|", &out));
}

void test_bb_reboot_record_decode_out_of_range_src(void)
{
    bb_reboot_record_t out;
    TEST_ASSERT_FALSE(bb_reboot_record_decode("999|0|0|", &out));
}

void test_bb_reboot_record_decode_truncates_oversized_detail(void)
{
    // A hand-crafted (not encode-produced) string with a detail field longer
    // than the 48-char struct field — exercises decode's own truncation
    // guard independent of encode's 48-char bound at write time (e.g.
    // corrupted/foreign NVS data).
    char str[128];
    snprintf(str, sizeof(str), "0|0|0|%060d", 0);  // 60 zero-padded digits
    TEST_ASSERT_EQUAL_INT(66, (int)strlen(str));

    bb_reboot_record_t out;
    TEST_ASSERT_TRUE(bb_reboot_record_decode(str, &out));
    TEST_ASSERT_EQUAL_INT(48, (int)strlen(out.detail));
}

// ---------------------------------------------------------------------------
// bb_reboot_pick_epoch — pure epoch-selection helper (B1-527 follow-up).
// ---------------------------------------------------------------------------

#define TEST_EPOCH_FLOOR 1704067200U  // 2024-01-01T00:00:00Z

void test_bb_reboot_pick_epoch_ntp_wins_over_caller(void)
{
    // NTP synced and above the floor wins even when a caller epoch is also
    // supplied.
    uint32_t got = bb_reboot_pick_epoch(true, TEST_EPOCH_FLOOR + 1000U,
                                         TEST_EPOCH_FLOOR + 1U, TEST_EPOCH_FLOOR);
    TEST_ASSERT_EQUAL_UINT32(TEST_EPOCH_FLOOR + 1000U, got);
}

void test_bb_reboot_pick_epoch_ntp_synced_but_below_floor_falls_to_caller(void)
{
    // ntp_synced=true but the device epoch itself is below the sanity
    // floor (e.g. RTC not yet set) — falls through to the caller epoch.
    uint32_t got = bb_reboot_pick_epoch(true, TEST_EPOCH_FLOOR - 1U,
                                         TEST_EPOCH_FLOOR + 5U, TEST_EPOCH_FLOOR);
    TEST_ASSERT_EQUAL_UINT32(TEST_EPOCH_FLOOR + 5U, got);
}

void test_bb_reboot_pick_epoch_caller_only(void)
{
    // No NTP sync at all — caller epoch (valid) is used.
    uint32_t got = bb_reboot_pick_epoch(false, 0U, TEST_EPOCH_FLOOR + 42U, TEST_EPOCH_FLOOR);
    TEST_ASSERT_EQUAL_UINT32(TEST_EPOCH_FLOOR + 42U, got);
}

void test_bb_reboot_pick_epoch_both_absent_returns_zero(void)
{
    uint32_t got = bb_reboot_pick_epoch(false, 0U, 0U, TEST_EPOCH_FLOOR);
    TEST_ASSERT_EQUAL_UINT32(0U, got);
}

void test_bb_reboot_pick_epoch_caller_below_floor_returns_zero(void)
{
    // NTP not synced and the caller-supplied epoch is below the sanity
    // floor (e.g. a client with its own unset clock) — neither is usable.
    uint32_t got = bb_reboot_pick_epoch(false, 0U, TEST_EPOCH_FLOOR - 1U, TEST_EPOCH_FLOOR);
    TEST_ASSERT_EQUAL_UINT32(0U, got);
}

// ---------------------------------------------------------------------------
// bb_system_reboot_parse_body — pure parse of POST /api/reboot's optional
// JSON body (B1-527 follow-up).
// ---------------------------------------------------------------------------

void test_bb_system_reboot_parse_body_null_body(void)
{
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(NULL, 0, NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
    TEST_ASSERT_EQUAL_STRING("", detail);
}

void test_bb_system_reboot_parse_body_empty_body(void)
{
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body("", 0, NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
    TEST_ASSERT_EQUAL_STRING("", detail);
}

void test_bb_system_reboot_parse_body_non_json(void)
{
    uint32_t ts = 999;
    char detail[49] = "unset";
    const char *body = "not json";
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
    TEST_ASSERT_EQUAL_STRING("", detail);
}

void test_bb_system_reboot_parse_body_oversized_garbage(void)
{
    // A large non-JSON blob — tolerated like any other parse failure.
    char body[512];
    memset(body, 'x', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
    TEST_ASSERT_EQUAL_STRING("", detail);
}

void test_bb_system_reboot_parse_body_detail_used(void)
{
    const char *body = "{\"detail\":\"manual restart\"}";
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(body, (int)strlen(body), "some-ua", &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
    TEST_ASSERT_EQUAL_STRING("manual restart", detail);
}

void test_bb_system_reboot_parse_body_empty_detail_falls_back_to_ua(void)
{
    const char *body = "{\"detail\":\"\"}";
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(body, (int)strlen(body), "curl/8.0", &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("curl/8.0", detail);
}

void test_bb_system_reboot_parse_body_no_detail_uses_ua(void)
{
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(NULL, 0, "curl/8.0", &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("curl/8.0", detail);
}

void test_bb_system_reboot_parse_body_no_detail_no_ua_empty(void)
{
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(NULL, 0, NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("", detail);
}

void test_bb_system_reboot_parse_body_ts_negative_zero(void)
{
    const char *body = "{\"ts\":-5}";
    uint32_t ts = 999;
    char detail[49];
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
}

void test_bb_system_reboot_parse_body_ts_zero_stays_zero(void)
{
    const char *body = "{\"ts\":0}";
    uint32_t ts = 999;
    char detail[49];
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
}

void test_bb_system_reboot_parse_body_ts_huge_returns_zero(void)
{
    const char *body = "{\"ts\":1e300}";
    uint32_t ts = 999;
    char detail[49];
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
}

void test_bb_system_reboot_parse_body_ts_over_uint32_max_returns_zero(void)
{
    const char *body = "{\"ts\":4294967296}";  // UINT32_MAX + 1
    uint32_t ts = 999;
    char detail[49];
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(0U, ts);
}

void test_bb_system_reboot_parse_body_ts_valid(void)
{
    const char *body = "{\"ts\":1735689600}";
    uint32_t ts = 999;
    char detail[49];
    bb_system_reboot_parse_body(body, (int)strlen(body), NULL, &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_UINT32(1735689600U, ts);
}

void test_bb_system_reboot_parse_body_ua_truncated_to_out_len(void)
{
    uint32_t ts = 999;
    char detail[8];
    bb_system_reboot_parse_body(NULL, 0, "a-very-long-user-agent-string", &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("a-very-", detail);  // 7 chars + NUL
}

void test_bb_system_reboot_parse_body_empty_ua_stays_empty(void)
{
    // ua_or_null is non-NULL but empty ("") — with no body detail either,
    // exercises the ua_or_null[0] sub-branch of the precedence check
    // (distinct from the ua_or_null==NULL case covered above).
    uint32_t ts = 999;
    char detail[49] = "unset";
    bb_system_reboot_parse_body(NULL, 0, "", &ts, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("", detail);
}

void test_bb_system_reboot_parse_body_guard_out_detail_len_zero_is_safe_noop(void)
{
    uint32_t ts = 999;
    char detail[8] = "unset";
    // Must not underflow/OOB-write; out_ts/out_detail are left untouched.
    bb_system_reboot_parse_body(NULL, 0, "curl/8.0", &ts, detail, 0);
    TEST_ASSERT_EQUAL_UINT32(999U, ts);
    TEST_ASSERT_EQUAL_STRING("unset", detail);
}

void test_bb_system_reboot_parse_body_guard_out_detail_null_is_safe_noop(void)
{
    uint32_t ts = 999;
    bb_system_reboot_parse_body(NULL, 0, "curl/8.0", &ts, NULL, 8);
    TEST_ASSERT_EQUAL_UINT32(999U, ts);
}

void test_bb_system_reboot_parse_body_guard_out_ts_null_is_safe_noop(void)
{
    char detail[8] = "unset";
    bb_system_reboot_parse_body(NULL, 0, "curl/8.0", NULL, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("unset", detail);
}

void test_bb_reboot_record_decode_leaves_out_untouched_on_failure(void)
{
    bb_reboot_record_t out;
    memset(&out, 0x5A, sizeof(out));
    TEST_ASSERT_FALSE(bb_reboot_record_decode("bad", &out));
    // untouched — every byte still 0x5A
    const uint8_t *bytes = (const uint8_t *)&out;
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, bytes[i]);
    }
}

// ---------------------------------------------------------------------------
// bb_reboot_history_push / _encode / _decode — rolling ring (B1-527 PR-B).
// ---------------------------------------------------------------------------

void test_bb_reboot_history_push_null_args(void)
{
    bb_reboot_history_t h = {0};
    bb_reboot_hist_entry_t e = {0};
    bb_reboot_history_push(NULL, &e);  // no-op, must not crash
    bb_reboot_history_push(&h, NULL);  // no-op, must not crash
    TEST_ASSERT_EQUAL_UINT8(0, h.count);
}

void test_bb_reboot_history_push_appends_below_capacity(void)
{
    bb_reboot_history_t h = {0};
    bb_reboot_hist_entry_t e1 = { .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = 100, .uptime_s = 10 };
    bb_reboot_hist_entry_t e2 = { .src = (uint8_t)BB_RESET_SRC_EGRESS_TIER3, .epoch_s = 200, .uptime_s = 20 };

    bb_reboot_history_push(&h, &e1);
    TEST_ASSERT_EQUAL_UINT8(1, h.count);
    TEST_ASSERT_EQUAL_UINT8(0, h.head);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BB_RESET_SRC_API_REBOOT, h.entries[0].src);

    bb_reboot_history_push(&h, &e2);
    TEST_ASSERT_EQUAL_UINT8(2, h.count);
    TEST_ASSERT_EQUAL_UINT8(0, h.head);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BB_RESET_SRC_EGRESS_TIER3, h.entries[1].src);
}

void test_bb_reboot_history_push_evicts_oldest_at_capacity(void)
{
    bb_reboot_history_t h = {0};
    for (uint8_t i = 0; i < BB_REBOOT_HISTORY_CAP; i++) {
        bb_reboot_hist_entry_t e = { .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = i, .uptime_s = i };
        bb_reboot_history_push(&h, &e);
    }
    TEST_ASSERT_EQUAL_UINT8(BB_REBOOT_HISTORY_CAP, h.count);
    TEST_ASSERT_EQUAL_UINT8(0, h.head);

    // One more push must evict the oldest (epoch_s==0) and advance head.
    bb_reboot_hist_entry_t newest = { .src = (uint8_t)BB_RESET_SRC_OTA_BOOT_DONE, .epoch_s = 999, .uptime_s = 99 };
    bb_reboot_history_push(&h, &newest);

    TEST_ASSERT_EQUAL_UINT8(BB_REBOOT_HISTORY_CAP, h.count);
    TEST_ASSERT_EQUAL_UINT8(1, h.head); // oldest slot advanced
    // The evicted slot (index 0) now holds the newest entry.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BB_RESET_SRC_OTA_BOOT_DONE, h.entries[0].src);
    TEST_ASSERT_EQUAL_UINT32(999U, h.entries[0].epoch_s);
    // The new oldest (index 1, epoch_s==1) is unchanged.
    TEST_ASSERT_EQUAL_UINT32(1U, h.entries[1].epoch_s);
}

void test_bb_reboot_history_encode_null_args(void)
{
    char buf[BB_REBOOT_HISTORY_STR_MAX];
    bb_reboot_history_t h = {0};
    TEST_ASSERT_FALSE(bb_reboot_history_encode(NULL, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(bb_reboot_history_encode(&h, NULL, sizeof(buf)));
    TEST_ASSERT_FALSE(bb_reboot_history_encode(&h, buf, 0));
}

void test_bb_reboot_history_encode_decode_roundtrip_partial(void)
{
    bb_reboot_history_t h = {0};
    bb_reboot_hist_entry_t e1 = { .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = 1735689600U, .uptime_s = 3600 };
    bb_reboot_hist_entry_t e2 = { .src = (uint8_t)BB_RESET_SRC_EGRESS_TIER3, .epoch_s = 1735693200U, .uptime_s = 60 };
    bb_reboot_history_push(&h, &e1);
    bb_reboot_history_push(&h, &e2);

    char buf[BB_REBOOT_HISTORY_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_history_encode(&h, buf, sizeof(buf)));

    bb_reboot_history_t out;
    memset(&out, 0xAA, sizeof(out));
    TEST_ASSERT_TRUE(bb_reboot_history_decode(buf, &out));
    TEST_ASSERT_EQUAL_UINT8(h.head,  out.head);
    TEST_ASSERT_EQUAL_UINT8(h.count, out.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BB_RESET_SRC_API_REBOOT, out.entries[0].src);
    TEST_ASSERT_EQUAL_UINT32(1735689600U, out.entries[0].epoch_s);
    TEST_ASSERT_EQUAL_UINT32(3600U, out.entries[0].uptime_s);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BB_RESET_SRC_EGRESS_TIER3, out.entries[1].src);
    TEST_ASSERT_EQUAL_UINT32(1735693200U, out.entries[1].epoch_s);
    TEST_ASSERT_EQUAL_UINT32(60U, out.entries[1].uptime_s);
}

void test_bb_reboot_history_encode_decode_roundtrip_full_wrapped(void)
{
    bb_reboot_history_t h = {0};
    for (uint8_t i = 0; i < BB_REBOOT_HISTORY_CAP + 3; i++) {
        bb_reboot_hist_entry_t e = { .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = i, .uptime_s = i };
        bb_reboot_history_push(&h, &e);
    }
    TEST_ASSERT_EQUAL_UINT8(BB_REBOOT_HISTORY_CAP, h.count);
    TEST_ASSERT_EQUAL_UINT8(3, h.head);

    char buf[BB_REBOOT_HISTORY_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_history_encode(&h, buf, sizeof(buf)));

    bb_reboot_history_t out;
    TEST_ASSERT_TRUE(bb_reboot_history_decode(buf, &out));
    TEST_ASSERT_EQUAL_UINT8(3, out.head);
    TEST_ASSERT_EQUAL_UINT8(BB_REBOOT_HISTORY_CAP, out.count);
    for (uint8_t i = 0; i < BB_REBOOT_HISTORY_CAP; i++) {
        TEST_ASSERT_EQUAL_UINT32(h.entries[i].epoch_s, out.entries[i].epoch_s);
    }
}

void test_bb_reboot_history_encode_decode_empty_ring(void)
{
    bb_reboot_history_t h = {0};

    char buf[BB_REBOOT_HISTORY_STR_MAX];
    TEST_ASSERT_TRUE(bb_reboot_history_encode(&h, buf, sizeof(buf)));

    bb_reboot_history_t out;
    TEST_ASSERT_TRUE(bb_reboot_history_decode(buf, &out));
    TEST_ASSERT_EQUAL_UINT8(0, out.head);
    TEST_ASSERT_EQUAL_UINT8(0, out.count);
}

void test_bb_reboot_history_encode_buffer_too_small(void)
{
    bb_reboot_history_t h = {0};
    bb_reboot_hist_entry_t e = { .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = 1, .uptime_s = 1 };
    bb_reboot_history_push(&h, &e);

    char tiny[4];
    TEST_ASSERT_FALSE(bb_reboot_history_encode(&h, tiny, sizeof(tiny)));
}

void test_bb_reboot_history_decode_null_args(void)
{
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode(NULL, &out));
    TEST_ASSERT_FALSE(bb_reboot_history_decode("0|0|", NULL));
}

void test_bb_reboot_history_decode_malformed(void)
{
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("", &out));
    TEST_ASSERT_FALSE(bb_reboot_history_decode("not-a-ring", &out));
    TEST_ASSERT_FALSE(bb_reboot_history_decode("1|2", &out));
}

void test_bb_reboot_history_decode_rejects_missing_trailing_pipe(void)
{
    // Two numeric fields with no trailing '|' satisfy both %u conversions
    // but never reach the %n — consumed stays 0 and decode must reject
    // rather than reading uninitialized/garbage entry data.
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("0|0", &out));
}

void test_bb_reboot_history_decode_rejects_leading_sign(void)
{
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("-1|0|", &out));
}

void test_bb_reboot_history_decode_out_of_range_head(void)
{
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("99|0|", &out));
}

void test_bb_reboot_history_decode_out_of_range_count(void)
{
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("0|99|", &out));
}

void test_bb_reboot_history_decode_out_of_range_entry_src(void)
{
    // Hand-crafted string: valid header, first entry has src=999.
    char str[BB_REBOOT_HISTORY_STR_MAX];
    int off = snprintf(str, sizeof(str), "0|0|");
    for (uint8_t i = 0; i < BB_REBOOT_HISTORY_CAP; i++) {
        off += snprintf(str + off, sizeof(str) - (size_t)off, "%s%u,0,0",
                         (i == 0) ? "" : ";", (i == 0) ? 999U : 0U);
    }
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode(str, &out));
}

void test_bb_reboot_history_decode_bad_entry_delimiter(void)
{
    // Valid header + first entry, but a comma instead of ';' between entries.
    char str[BB_REBOOT_HISTORY_STR_MAX];
    int off = snprintf(str, sizeof(str), "0|0|0,0,0,0,0,0");
    (void)off;
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode(str, &out));
}

void test_bb_reboot_history_encode_buffer_too_small_mid_loop(void)
{
    // Buffer fits the header + first entry, but not the second — exercises
    // the loop-internal (size_t)n >= remaining check, distinct from the
    // header-only "too small" case above.
    bb_reboot_history_t h = {0};
    bb_reboot_hist_entry_t e1 = { .src = 0, .epoch_s = 0, .uptime_s = 0 };
    bb_reboot_hist_entry_t e2 = { .src = 0, .epoch_s = 0, .uptime_s = 0 };
    bb_reboot_history_push(&h, &e1);
    bb_reboot_history_push(&h, &e2);

    char buf[10];
    TEST_ASSERT_FALSE(bb_reboot_history_encode(&h, buf, sizeof(buf)));
}

void test_bb_reboot_history_decode_header_field_count_mismatch(void)
{
    // "5|" — first %u matches, second %u has nothing to consume, so sscanf
    // returns 1 (not 2). Distinct from the consumed==0 (missing trailing
    // pipe) branch tested above.
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("5|", &out));
}

void test_bb_reboot_history_decode_entry_field_mismatch(void)
{
    // Valid header, but the first entry's fields don't start with a digit —
    // the per-entry sscanf returns 0 (not 3).
    bb_reboot_history_t out;
    TEST_ASSERT_FALSE(bb_reboot_history_decode("0|0|bad", &out));
}

void test_bb_reboot_history_decode_leaves_out_untouched_on_failure(void)
{
    bb_reboot_history_t out;
    memset(&out, 0x5A, sizeof(out));
    TEST_ASSERT_FALSE(bb_reboot_history_decode("bad", &out));
    const uint8_t *bytes = (const uint8_t *)&out;
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, bytes[i]);
    }
}

// ---------------------------------------------------------------------------
// bb_system_boot_count_increment/_reset — boot-health counter (B1-753).
// Host storage is in-memory (s_boot_count); setUp() calls
// bb_system_boot_count_reset_for_test() so each test starts at 0. The public
// getter (bb_system_boot_count_get) and BB_SYSTEM_BOOT_FAIL_THRESHOLD were
// removed as dead code (nothing read them in production), so these tests
// only exercise the return-code contract of the two functions still in use.
// ---------------------------------------------------------------------------

void test_bb_system_boot_count_increment_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_system_boot_count_increment());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_system_boot_count_increment());
}

void test_bb_system_boot_count_reset_returns_ok(void)
{
    bb_system_boot_count_increment();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_system_boot_count_reset());
}

void test_bb_system_boot_count_increment_does_not_overflow(void)
{
    for (int i = 0; i < 260; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK, bb_system_boot_count_increment());
    }
}

// ---------------------------------------------------------------------------
// bb_system_boot_banner_format — pure CONFIG_BB_SYSTEM_BOOT_BANNER line formatter
// ---------------------------------------------------------------------------

void test_bb_system_boot_banner_format_all_present(void)
{
    char buf[128];
    int n = bb_system_boot_banner_format(buf, sizeof(buf), "myapp", "1.2.3",
                                          "Jan  1 2025", "12:00:00", "5.1.2");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("project=myapp version=1.2.3 build=Jan  1 2025 12:00:00 idf=5.1.2", buf);
}

void test_bb_system_boot_banner_format_all_null(void)
{
    char buf[128];
    int n = bb_system_boot_banner_format(buf, sizeof(buf), NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("project=? version=? build=? ? idf=?", buf);
}

void test_bb_system_boot_banner_format_null_out(void)
{
    TEST_ASSERT_EQUAL(-1, bb_system_boot_banner_format(NULL, 128, "a", "b", "c", "d", "e"));
}

void test_bb_system_boot_banner_format_zero_len(void)
{
    char buf[4];
    TEST_ASSERT_EQUAL(-1, bb_system_boot_banner_format(buf, 0, "a", "b", "c", "d", "e"));
}

void test_bb_system_boot_banner_format_truncation(void)
{
    char buf[8];
    int n = bb_system_boot_banner_format(buf, sizeof(buf), "myapp", "1.2.3",
                                          "Jan  1 2025", "12:00:00", "5.1.2");
    TEST_ASSERT_TRUE(n >= (int)sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
}
