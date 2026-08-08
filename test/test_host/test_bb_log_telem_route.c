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

// ---------------------------------------------------------------------------
// bb_log_telem_route_set/get -- the runtime gate bb_log_emit() reads via
// bb_log_telem_route_get() before calling bb_log_telem_route_wide() directly
// with the call site's own tag (B1-1443 PR-2: the former line-parsing
// combinator bb_log_telem_should_route_wide() was deleted along with
// bb_log_line_parse() -- see bb_log_internal.h). Exercises the same
// route_wide decision core as above, driven through the runtime gate rather
// than a literal bool, so the toggle itself is proven to affect the outcome.
// ---------------------------------------------------------------------------

void test_bb_log_telem_route_set_toggles_runtime_gate(void) {
    bb_log_telem_route_set(false);
    TEST_ASSERT_FALSE(bb_log_telem_route_get());
    TEST_ASSERT_FALSE(bb_log_telem_route_wide(bb_log_telem_route_get(), "TELEM"));

    bb_log_telem_route_set(true);
    TEST_ASSERT_TRUE(bb_log_telem_route_get());
    TEST_ASSERT_TRUE(bb_log_telem_route_wide(bb_log_telem_route_get(), "TELEM"));

    bb_log_telem_route_set(false);  // restore default for later tests
    TEST_ASSERT_FALSE(bb_log_telem_route_get());
}
