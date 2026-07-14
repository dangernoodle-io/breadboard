// B1-267 Part A — manual timezone
// B1-750 — timezone moved from bb_nv to bb_settings (bb_nv dissolution
// epic B1-708); bb_settings' accessors forward to bb_config_get_str/
// set_str over the SAME "bb_cfg" NVS namespace + "timezone" key bb_nv used
// (see platform/host/bb_settings/bb_settings.c's BB_SETTINGS_TIMEZONE_KEY
// comment) -- byte-compat with already-provisioned boards.
//
// Tests:
//   1. bb_settings_timezone_set / bb_settings_timezone_get round-trip
//   2. bb_settings_timezone_set validation (NULL, empty, too-long)
//   3. byte-compat: the legacy "bb_cfg"/"timezone" address is the SAME one
//      bb_settings_timezone_get/set read/write (literal-address BITE test)
//   4. bb_ntp_set_timezone: persists AND applies (setenv/tzset)
//   5. bb_ntp_apply_saved_timezone: UTC default when empty
//   6. DST correctness: EST vs EDT on a known POSIX TZ string
//   7. Clear timezone reverts to UTC

#include "unity.h"
#include "bb_settings.h"
#include "bb_ntp.h"
#include "bb_storage.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// Known UTC epoch values (computed via calendar.timegm, not mktime):
//   2024-01-15 12:00:00 UTC — clearly in winter (EST, UTC-5) -> local 07:00
//   2024-07-04 12:00:00 UTC — clearly in summer (EDT, UTC-4) -> local 08:00
static const time_t T_WINTER_UTC = 1705320000;
static const time_t T_SUMMER_UTC = 1720094400;

// Test-local field descriptor targeting the exact same addr (backend/ns/key)
// as bb_settings.c's internal s_timezone_field -- the literal legacy
// address bb_nv_config used, kept hardcoded here (NOT via bb_settings' own
// field descriptor) so the byte-compat contract is executable, mirroring
// test_bb_settings_creds_write.c's pattern.
static const bb_config_field_t s_test_timezone_field = {
    .id      = "timezone",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "timezone" },
    .max_len = 65,
};

static void reset_state(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
    // clear TZ env so each test starts from UTC
    setenv("TZ", "UTC0", 1);
    tzset();
}

// ---------------------------------------------------------------------------
// bb_settings_timezone_get/set round-trip
// ---------------------------------------------------------------------------

void test_settings_timezone_default_is_empty(void)
{
    reset_state();
    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_EQUAL_STRING("", tz);
}

void test_settings_timezone_set_stores_and_reads_back(void)
{
    reset_state();
    bb_err_t err = bb_settings_timezone_set("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", tz);
}

void test_settings_timezone_set_null_clears(void)
{
    reset_state();
    bb_settings_timezone_set("EST5EDT,M3.2.0,M11.1.0");
    bb_err_t err = bb_settings_timezone_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("", tz);
}

void test_settings_timezone_set_empty_string_clears(void)
{
    reset_state();
    bb_settings_timezone_set("PST8");
    bb_err_t err = bb_settings_timezone_set("");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("", tz);
}

void test_settings_timezone_set_too_long_returns_invalid_arg(void)
{
    reset_state();
    // 65 printable chars — one over the 64-char max
    char too_long[66];
    memset(too_long, 'A', 65);
    too_long[65] = '\0';
    bb_err_t err = bb_settings_timezone_set(too_long);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);

    // value must not have changed
    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("", tz);
}

void test_settings_timezone_set_max_len_succeeds(void)
{
    reset_state();
    // exactly 64 chars — should succeed
    char max_len[65];
    memset(max_len, 'A', 64);
    max_len[64] = '\0';
    bb_err_t err = bb_settings_timezone_set(max_len);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING(max_len, tz);
}

// ---------------------------------------------------------------------------
// Byte-compat (GATE 1, B1-750): the legacy "bb_cfg"/"timezone" address IS
// the address bb_settings_timezone_get/set read/write -- BITE test, not just
// documentation. Seeding through the literal legacy field must be visible
// via the real accessor, and vice versa.
// ---------------------------------------------------------------------------

void test_settings_timezone_byte_compat_legacy_address_readable_via_accessor(void)
{
    reset_state();
    // Seed directly at the literal legacy ns/key (bypassing bb_settings.c's
    // own field descriptor entirely).
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_timezone_field, "PST8PDT"));

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("PST8PDT", tz);
}

void test_settings_timezone_byte_compat_accessor_write_readable_via_legacy_address(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_set("MST7MDT"));

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_timezone_field, tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("MST7MDT", tz);
}

// ---------------------------------------------------------------------------
// bb_ntp_apply_saved_timezone — UTC default when nothing stored
// ---------------------------------------------------------------------------

void test_ntp_apply_saved_timezone_defaults_to_utc(void)
{
    reset_state();
    // nothing stored → should apply UTC0
    bb_ntp_apply_saved_timezone();

    struct tm tm_out;
    localtime_r(&T_WINTER_UTC, &tm_out);
    // In UTC: 2024-01-15 12:00:00 -> hour=12
    TEST_ASSERT_EQUAL_INT(12, tm_out.tm_hour);
    TEST_ASSERT_EQUAL_INT(0,  tm_out.tm_min);
    TEST_ASSERT_EQUAL_INT(0,  tm_out.tm_isdst);
}

// ---------------------------------------------------------------------------
// bb_ntp_set_timezone — persists and applies immediately
// ---------------------------------------------------------------------------

void test_ntp_set_timezone_persists_to_settings(void)
{
    reset_state();
    bb_err_t err = bb_ntp_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", tz);
}

void test_ntp_set_timezone_applies_immediately(void)
{
    reset_state();
    bb_err_t err = bb_ntp_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // Verify via localtime_r: 2024-01-15 12:00:00 UTC → 07:00 EST (winter)
    struct tm tm_out;
    localtime_r(&T_WINTER_UTC, &tm_out);
    TEST_ASSERT_EQUAL_INT(7, tm_out.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, tm_out.tm_min);
    TEST_ASSERT_EQUAL_INT(0, tm_out.tm_isdst);  // standard time
}

void test_ntp_set_timezone_null_clears_to_utc(void)
{
    reset_state();
    bb_ntp_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    bb_err_t err = bb_ntp_set_timezone(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("", tz);

    // clock should now be UTC
    struct tm tm_out;
    localtime_r(&T_WINTER_UTC, &tm_out);
    TEST_ASSERT_EQUAL_INT(12, tm_out.tm_hour);
    TEST_ASSERT_EQUAL_INT(0,  tm_out.tm_isdst);
}

void test_ntp_set_timezone_empty_string_clears_to_utc(void)
{
    reset_state();
    bb_ntp_set_timezone("PST8");
    bb_err_t err = bb_ntp_set_timezone("");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    char tz[65] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_timezone_get(tz, sizeof(tz), &len));
    TEST_ASSERT_EQUAL_STRING("", tz);

    struct tm tm_out;
    localtime_r(&T_WINTER_UTC, &tm_out);
    TEST_ASSERT_EQUAL_INT(12, tm_out.tm_hour);
}

// ---------------------------------------------------------------------------
// DST correctness: EST in winter, EDT in summer
// ---------------------------------------------------------------------------

void test_ntp_dst_winter_is_est(void)
{
    reset_state();
    bb_ntp_set_timezone("EST5EDT,M3.2.0,M11.1.0");

    // 2024-01-15 12:00:00 UTC → 07:00:00 EST (UTC-5, dst=0)
    struct tm tm_out;
    localtime_r(&T_WINTER_UTC, &tm_out);
    TEST_ASSERT_EQUAL_INT(7, tm_out.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, tm_out.tm_min);
    TEST_ASSERT_EQUAL_INT(0, tm_out.tm_isdst);  // standard time
}

void test_ntp_dst_summer_is_edt(void)
{
    reset_state();
    bb_ntp_set_timezone("EST5EDT,M3.2.0,M11.1.0");

    // 2024-07-04 12:00:00 UTC → 08:00:00 EDT (UTC-4, dst=1)
    struct tm tm_out;
    localtime_r(&T_SUMMER_UTC, &tm_out);
    TEST_ASSERT_EQUAL_INT(8,  tm_out.tm_hour);
    TEST_ASSERT_EQUAL_INT(0,  tm_out.tm_min);
    TEST_ASSERT_EQUAL_INT(1,  tm_out.tm_isdst);  // daylight saving time
}

// ---------------------------------------------------------------------------
// bb_ntp_apply_saved_timezone reads stored value on boot
// ---------------------------------------------------------------------------

void test_ntp_apply_saved_timezone_reads_stored_value(void)
{
    reset_state();
    // Store tz via bb_settings directly, then call apply
    bb_settings_timezone_set("EST5EDT,M3.2.0,M11.1.0");
    // Reset TZ env to UTC first so we can verify apply actually changes it
    setenv("TZ", "UTC0", 1);
    tzset();

    bb_ntp_apply_saved_timezone();

    struct tm tm_out;
    localtime_r(&T_WINTER_UTC, &tm_out);
    // Should now be EST (UTC-5) -> hour 7
    TEST_ASSERT_EQUAL_INT(7, tm_out.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, tm_out.tm_isdst);
}
