// B1-267 Part A — manual timezone
//
// Tests:
//   1. bb_nv bb_nv_config_set_timezone / bb_nv_config_timezone round-trip
//   2. bb_nv validation (NULL, empty, too-long)
//   3. bb_nv factory_reset clears timezone
//   4. bb_ntp_set_timezone: persists AND applies (setenv/tzset)
//   5. bb_ntp_apply_saved_timezone: UTC default when empty
//   6. DST correctness: EST vs EDT on a known POSIX TZ string
//   7. Clear timezone reverts to UTC

#include "unity.h"
#include "bb_nv.h"
#include "bb_ntp.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// Known UTC epoch values (computed via calendar.timegm, not mktime):
//   2024-01-15 12:00:00 UTC — clearly in winter (EST, UTC-5) -> local 07:00
//   2024-07-04 12:00:00 UTC — clearly in summer (EDT, UTC-4) -> local 08:00
static const time_t T_WINTER_UTC = 1705320000;
static const time_t T_SUMMER_UTC = 1720094400;

static void reset_state(void)
{
    bb_nv_config_init();
    bb_nv_config_factory_reset();
    // clear TZ env so each test starts from UTC
    setenv("TZ", "UTC0", 1);
    tzset();
}

// ---------------------------------------------------------------------------
// bb_nv round-trip
// ---------------------------------------------------------------------------

void test_nv_timezone_default_is_empty(void)
{
    reset_state();
    const char *tz = bb_nv_config_timezone();
    TEST_ASSERT_NOT_NULL(tz);
    TEST_ASSERT_EQUAL_STRING("", tz);
}

void test_nv_set_timezone_stores_and_reads_back(void)
{
    reset_state();
    bb_err_t err = bb_nv_config_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", bb_nv_config_timezone());
}

void test_nv_set_timezone_null_clears(void)
{
    reset_state();
    bb_nv_config_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    bb_err_t err = bb_nv_config_set_timezone(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_timezone());
}

void test_nv_set_timezone_empty_string_clears(void)
{
    reset_state();
    bb_nv_config_set_timezone("PST8");
    bb_err_t err = bb_nv_config_set_timezone("");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_timezone());
}

void test_nv_set_timezone_too_long_returns_invalid_arg(void)
{
    reset_state();
    // 65 printable chars — one over the 64-char max
    char too_long[66];
    memset(too_long, 'A', 65);
    too_long[65] = '\0';
    bb_err_t err = bb_nv_config_set_timezone(too_long);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
    // value must not have changed
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_timezone());
}

void test_nv_set_timezone_max_len_succeeds(void)
{
    reset_state();
    // exactly 64 chars — should succeed
    char max_len[65];
    memset(max_len, 'A', 64);
    max_len[64] = '\0';
    bb_err_t err = bb_nv_config_set_timezone(max_len);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING(max_len, bb_nv_config_timezone());
}

void test_nv_factory_reset_clears_timezone(void)
{
    reset_state();
    bb_nv_config_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", bb_nv_config_timezone());

    bb_nv_config_factory_reset();
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_timezone());
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

void test_ntp_set_timezone_persists_to_nv(void)
{
    reset_state();
    bb_err_t err = bb_ntp_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", bb_nv_config_timezone());
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
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_timezone());

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
    TEST_ASSERT_EQUAL_STRING("", bb_nv_config_timezone());

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
    // Store tz via nv directly, then call apply
    bb_nv_config_set_timezone("EST5EDT,M3.2.0,M11.1.0");
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
