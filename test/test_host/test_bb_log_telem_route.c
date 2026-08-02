#include "unity.h"
#include "bb_log_internal.h"

// ---------------------------------------------------------------------------
// bb_log_telem_route_wide -- TELEM wide-routing decision core. Host build
// default: BB_LOG_TELEM_TAG_STR="TELEM" (matches the Kconfig default, no
// sdkconfig.h on host so the C fallback is what's exercised here).
// ---------------------------------------------------------------------------

void test_bb_log_telem_route_wide_events_disabled_telem_tag_stays_console_only(void) {
    TEST_ASSERT_FALSE(bb_log_telem_route_wide(false, "TELEM"));
}

void test_bb_log_telem_route_wide_events_enabled_routes_wide_regardless_of_tag(void) {
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(true, "TELEM"));
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(true, "wifi"));
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(true, NULL));
}

void test_bb_log_telem_route_wide_non_telem_tag_routes_wide_events_disabled(void) {
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(false, "wifi"));
}

void test_bb_log_telem_route_wide_non_telem_tag_routes_wide_events_enabled(void) {
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(true, "wifi"));
}

void test_bb_log_telem_route_wide_null_tag_routes_wide(void) {
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(false, NULL));
}

void test_bb_log_telem_route_wide_empty_tag_routes_wide(void) {
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(false, ""));
}

void test_bb_log_telem_route_wide_superstring_tag_routes_wide(void) {
    // "TELEMETRY" is a different tag from "TELEM" -- a naive substring
    // compare (strstr/strncmp on the shorter length) would wrongly treat
    // this as a match. Must route wide.
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(false, "TELEMETRY"));
}

void test_bb_log_telem_route_wide_prefix_tag_routes_wide(void) {
    // "TELE" is a prefix of "TELEM", not an exact match -- must route wide.
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(false, "TELE"));
}

void test_bb_log_telem_route_wide_case_sensitive_mismatch_routes_wide(void) {
    // Comparison is exact/case-sensitive -- "telem" != "TELEM".
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(false, "telem"));
}
