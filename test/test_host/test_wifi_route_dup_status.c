// Host exercise of bb_wifi_http_route_register_outcome() (B1-1259) -- the
// pure bb_err_t -> bb_err_t mapper bb_wifi_routes_init()'s
// register_route_tolerate_dup() delegates to
// (platform/espidf/bb_wifi_http/bb_wifi_http_routes.c). That caller chain is
// ESP-IDF-only (its component REQUIRES esp_wifi, and it includes
// <esp_wifi.h> unconditionally) and cannot link on host -- this file drives
// the SAME production mapping function directly instead, mirroring
// test_wifi_creds_apply_route.c's own doc comment for
// bb_wifi_http_status_for_apply_rc().

#include "unity.h"

#include "bb_wifi_http_route_dup_status.h"

// A duplicate (method,path) registration -- BB_ERR_INVALID_STATE, the
// outcome bb_dispatch_api_add() returns for "first registration wins" -- is
// downgraded to BB_OK: a benign, tolerated outcome, not a bundle-aborting
// failure.
void test_wifi_http_route_register_outcome_dup_maps_to_ok(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_route_register_outcome(BB_ERR_INVALID_STATE));
}

// Success passes through unchanged.
void test_wifi_http_route_register_outcome_ok_stays_ok(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_route_register_outcome(BB_OK));
}

// A genuine per-route failure (e.g. the dispatch table is full) is NOT
// downgraded -- bb_wifi_routes_init() must still abort the whole bundle on
// this, distinct from the tolerated duplicate case above.
void test_wifi_http_route_register_outcome_no_space_stays_fatal(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_wifi_http_route_register_outcome(BB_ERR_NO_SPACE));
}

// Any other non-BB_OK, non-duplicate error also passes through unchanged --
// only BB_ERR_INVALID_STATE is special-cased.
void test_wifi_http_route_register_outcome_other_error_stays_fatal(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_wifi_http_route_register_outcome(BB_ERR_INVALID_ARG));
}
