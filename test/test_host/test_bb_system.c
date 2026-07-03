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
