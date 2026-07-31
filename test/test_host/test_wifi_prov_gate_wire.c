#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_http_prov_gate.h"
#include "wifi_prov_gate_wire.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// B1-1279 PR-4 -- activation tests. wifi_prov_gate_wire_register() is the
// SAME pure function platform/espidf/bb_wifi_prov/bb_wifi_prov.c calls in
// production (bb_wifi_prov_start()); this file is its one host-testable
// seam, exercised through the SAME bb_http_host_* dispatch mirrors PR-3's
// test_bb_http_prov_gate_wire.c already validated the gate mechanism
// through, so these tests prove the actual PRODUCTION allowlist content is
// correct, not just the gate mechanism in the abstract.
//
// setUp() (test_main.c) resets the allowlist + injected active-fn
// (bb_http_prov_gate_reset()) before every test.
// ---------------------------------------------------------------------------

static bool s_mock_prov_active;

static bool mock_prov_active_fn(void)
{
    return s_mock_prov_active;
}

static bool s_stub_invoked;

static bb_err_t counting_stub_handler(bb_http_request_t *req)
{
    s_stub_invoked = true;
    bb_http_resp_set_status(req, 200);
    return BB_OK;
}

static const uint8_t s_index_html[] = { '<', 'h', 'i', '>' };
static const uint8_t s_theme_css[]  = { 'b', 'o', 'd', 'y' };

static const bb_http_asset_t s_single_asset[] = {
    { .path = "/", .mime = "text/html", .encoding = NULL,
      .data = s_index_html, .len = sizeof(s_index_html) },
};

static const bb_http_asset_t s_multi_assets[] = {
    { .path = "/", .mime = "text/html", .encoding = NULL,
      .data = s_index_html, .len = sizeof(s_index_html) },
    { .path = "/theme.css", .mime = "text/css", .encoding = NULL,
      .data = s_theme_css, .len = sizeof(s_theme_css) },
};

static void arm_mock_active_fn(void)
{
    s_mock_prov_active = false;
    bb_http_prov_gate_set_active_fn(mock_prov_active_fn);
}

static void teardown(void)
{
    bb_http_prov_gate_set_active_fn(NULL);
    bb_dispatch_api_reset();
    bb_http_host_reset_routes();
    bb_http_host_reset_assets();
    wifi_prov_gate_wire_reset_for_test();
    s_stub_invoked = false;
}

// ===========================================================================
// Allowlist content -- the pure predicate, no dispatch involved.
// ===========================================================================

void test_wifi_prov_gate_wire_register_allows_fixed_and_single_asset(void)
{
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/save"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/api/wifi/scan"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/"));

    teardown();
}

void test_wifi_prov_gate_wire_register_allows_every_custom_asset_path(void)
{
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_multi_assets, 2));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/theme.css"));

    teardown();
}

void test_wifi_prov_gate_wire_register_zero_assets_still_allows_fixed_entries(void)
{
    // Matches bb_http_register_assets_with_fallback's own contract: assets
    // may be NULL only when n == 0 (a fallback-only / no-caller-UI portal).
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(NULL, 0));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/save"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/api/wifi/scan"));

    teardown();
}

void test_wifi_prov_gate_wire_register_does_not_allow_extra_route(void)
{
    // extra() consumer routes (advanced-UI backing routes) are deliberately
    // NOT part of the base provisioning-required set -- see
    // wifi_prov_gate_wire.h.
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/hardware"));

    teardown();
}

// A full allowlist cap (BB_HTTP_PROV_ALLOWLIST_CAP, default 8) means every
// bb_http_prov_allow() call this function makes returns BB_ERR_NO_SPACE --
// covers the failure branch on all four registration sites (the fixed
// GET /ping, POST /save, POST /api/wifi/scan calls, plus the per-asset
// loop) and confirms best-effort semantics: every entry is still attempted
// (no early return), and the FIRST failure is what's returned.
void test_wifi_prov_gate_wire_register_full_cap_returns_first_error_best_effort(void)
{
    static const char *dummy_paths[] = {
        "/dummy0", "/dummy1", "/dummy2", "/dummy3",
        "/dummy4", "/dummy5", "/dummy6", "/dummy7",
    };
    for (size_t i = 0; i < sizeof(dummy_paths) / sizeof(dummy_paths[0]); i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, dummy_paths[i]));
    }

    bb_err_t result = wifi_prov_gate_wire_register(s_single_asset, 1);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, result);

    // None of the four entries this call attempted actually fit.
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/save"));
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/api/wifi/scan"));
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/"));

    teardown();
}

// MEDIUM review fix (B1-1279 PR-4 finding): a cap-overflowing registration
// (e.g. a custom-branded portal whose asset count pushes fixed(3)+n past
// BB_HTTP_PROV_ALLOWLIST_CAP) must leave a queryable trace beyond the
// bb_err_t return the one production caller (bb_wifi_prov_start()) used to
// only log and discard. Same setup as the best-effort test above, but
// asserts the distinct completeness signal instead of (or in addition to)
// the return code.
void test_wifi_prov_gate_wire_register_cap_overflow_marks_registration_incomplete(void)
{
    static const char *dummy_paths[] = {
        "/dummy0", "/dummy1", "/dummy2", "/dummy3",
        "/dummy4", "/dummy5", "/dummy6", "/dummy7",
    };
    for (size_t i = 0; i < sizeof(dummy_paths) / sizeof(dummy_paths[0]); i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, dummy_paths[i]));
    }

    // Vacuously true before any wifi_prov_gate_wire_register() call.
    TEST_ASSERT_TRUE(wifi_prov_gate_wire_last_registration_complete());

    bb_err_t result = wifi_prov_gate_wire_register(s_single_asset, 1);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, result);
    TEST_ASSERT_FALSE(wifi_prov_gate_wire_last_registration_complete());

    teardown();
}

// The completeness signal isn't just "sticky false" -- a fresh, fully
// successful registration (an under-cap composition) reports true again.
void test_wifi_prov_gate_wire_register_under_cap_marks_registration_complete(void)
{
    bb_err_t result = wifi_prov_gate_wire_register(s_single_asset, 1);
    TEST_ASSERT_EQUAL(BB_OK, result);
    TEST_ASSERT_TRUE(wifi_prov_gate_wire_last_registration_complete());

    teardown();
}

// ===========================================================================
// Real production dispatch -- allowlisted routes reach their handlers while
// active.
// ===========================================================================

void test_wifi_prov_gate_wire_active_allowlisted_save_reaches_handler(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_POST, "/save", counting_stub_handler);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_POST, "/save");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_wifi_prov_gate_wire_active_allowlisted_scan_reaches_handler(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_POST, "/api/wifi/scan", counting_stub_handler);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_POST, "/api/wifi/scan");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_wifi_prov_gate_wire_active_allowlisted_ping_reaches_handler(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_GET, "/ping", counting_stub_handler);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_wifi_prov_gate_wire_active_allowlisted_index_asset_serves(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_assets(NULL, s_single_asset, 1);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_EQUAL(sizeof(s_index_html), cap.body_len);

    bb_http_host_capture_free(&cap);
    teardown();
}

// ===========================================================================
// Real production dispatch -- representative NON-allowlisted routes deny
// while active. Handler-not-invoked is asserted, not just the status code.
// ===========================================================================

// The route confirmed leaking on hardware (B1-1279's motivating exposure).
void test_wifi_prov_gate_wire_active_diag_meminfo_denied_and_handler_not_invoked(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/diag/meminfo", counting_stub_handler);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/diag/meminfo");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(s_stub_invoked);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

// A second /api/* route -- proves the denial isn't diag-specific.
void test_wifi_prov_gate_wire_active_api_reboot_denied_and_handler_not_invoked(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_POST, "/api/reboot", counting_stub_handler);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_POST, "/api/reboot");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(s_stub_invoked);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

// A non-/api route -- proves the denial isn't scoped to /api/* only.
void test_wifi_prov_gate_wire_active_non_api_route_denied_and_handler_not_invoked(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_GET, "/debug", counting_stub_handler);
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/debug");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(s_stub_invoked);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

// ===========================================================================
// Inactive: same three non-allowlisted routes are unaffected (reach their
// handlers normally) when provisioning is not active.
// ===========================================================================

void test_wifi_prov_gate_wire_inactive_diag_meminfo_unaffected(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/diag/meminfo", counting_stub_handler);
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/diag/meminfo");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_wifi_prov_gate_wire_inactive_api_reboot_unaffected(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_POST, "/api/reboot", counting_stub_handler);
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_POST, "/api/reboot");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}

void test_wifi_prov_gate_wire_inactive_non_api_route_unaffected(void)
{
    arm_mock_active_fn();
    TEST_ASSERT_EQUAL(BB_OK, wifi_prov_gate_wire_register(s_single_asset, 1));
    bb_http_register_route(NULL, BB_HTTP_GET, "/debug", counting_stub_handler);
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/debug");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown();
}
