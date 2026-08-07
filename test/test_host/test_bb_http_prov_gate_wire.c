#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_http_prov_gate.h"
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// B1-1279 PR3 — wiring tests. Exercises the SAME three insertion points as
// the device handlers in platform/espidf/bb_http_server/bb_http.c
// (api_dispatch_handler, bb_shim_handler, asset_wildcard_handler) through
// their host mirrors (bb_http_host_api_dispatch, bb_http_host_dispatch_route,
// bb_http_host_asset_wildcard) — NOT just the underlying pure
// bb_http_prov_gate_allow() predicate (already covered by
// test_bb_http_prov_gate.c). A suite that only exercised
// bb_http_prov_gate_allow() directly would miss a call site that forgot to
// invoke it.
//
// setUp() (test_main.c) already resets the allowlist + injected active-fn
// before every test (bb_http_prov_gate_reset()), so every test below starts
// from a clean, inactive gate.
// ---------------------------------------------------------------------------

static bool s_mock_prov_active;

static bool mock_prov_active_fn(void)
{
    return s_mock_prov_active;
}

static bool s_stub_handler_invoked;

static bb_err_t counting_stub_handler(bb_http_request_t *req)
{
    s_stub_handler_invoked = true;
    bb_http_resp_set_status(req, 200);
    return BB_OK;
}

static const uint8_t s_index_html[] = { '<', 'h', 'i', '>' };

static const bb_http_asset_t s_assets[] = {
    { .path = "/", .mime = "text/html", .encoding = NULL,
      .data = s_index_html, .len = sizeof(s_index_html) },
};

static void arm_mock_active_fn(void)
{
    s_mock_prov_active = false;
    bb_http_prov_gate_set_active_fn(mock_prov_active_fn);
}

static void teardown_wire_test(void)
{
    bb_http_prov_gate_set_active_fn(NULL);
    bb_dispatch_api_reset();
    bb_http_host_reset_routes();
    bb_http_host_reset_assets();
    s_stub_handler_invoked = false;
}

// ===========================================================================
// Insertion point 1/3 — api_dispatch_handler (host mirror:
// bb_http_host_api_dispatch)
// ===========================================================================

void test_bb_http_prov_gate_wire_api_active_allowlisted_reaches_handler(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/diag/meminfo", counting_stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/meminfo"));
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/diag/meminfo");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_api_active_non_allowlisted_denies_404(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/diag/meminfo", counting_stub_handler);
    // deliberately NOT allowlisted
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/diag/meminfo");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_api_inactive_unaffected(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_GET, "/api/diag/meminfo", counting_stub_handler);
    // gate never armed active; not allowlisted either -- must still reach handler
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_api_dispatch(req, BB_HTTP_GET, "/api/diag/meminfo");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

// ===========================================================================
// Insertion point 2/3 — bb_shim_handler (host mirror:
// bb_http_host_dispatch_route). Uses /ping, the policy's canonical
// always-reachable example (see bb_http_prov_gate.h / test_bb_http_prov_gate.c).
// ===========================================================================

void test_bb_http_prov_gate_wire_shim_active_ping_allowlisted_reaches_handler(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_GET, "/ping", counting_stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_shim_active_non_allowlisted_denies_404(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_POST, "/save", counting_stub_handler);
    // deliberately NOT allowlisted
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_POST, "/save");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_shim_inactive_unaffected(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_POST, "/save", counting_stub_handler);
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_POST, "/save");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

// ===========================================================================
// Insertion point 3/3 — asset_wildcard_handler (host mirror:
// bb_http_host_asset_wildcard, gates internally)
// ===========================================================================

void test_bb_http_prov_gate_wire_asset_active_allowlisted_serves(void)
{
    arm_mock_active_fn();
    bb_http_register_assets(NULL, &(bb_http_register_assets_cfg_t){ .assets = s_assets, .n = 1 });
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/"));
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
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_asset_active_non_allowlisted_denies_404(void)
{
    arm_mock_active_fn();
    bb_http_register_assets(NULL, &(bb_http_register_assets_cfg_t){ .assets = s_assets, .n = 1 });
    // deliberately NOT allowlisted
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);
    // Same status/shape as a genuinely-unknown asset -- indistinguishable
    // from a nonexistent route (policy requirement).
    TEST_ASSERT_NULL(cap.body);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_asset_inactive_unaffected(void)
{
    arm_mock_active_fn();
    bb_http_register_assets(NULL, &(bb_http_register_assets_cfg_t){ .assets = s_assets, .n = 1 });
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

// ===========================================================================
// Captive-portal fallback: a MISS on the asset table must reach the
// registered fallback (e.g. bb_wifi_prov's redirect) UNGATED while
// provisioning is active. Client OSes probe unenumerable paths
// (/generate_204, /hotspot-detect.html, …) to auto-detect the captive
// portal; bb_http_prov_allow() rejects any non-/api wildcard entry, so
// those paths could never be allowlisted directly — the fallback is the
// only way they can ever be reachable. This proves the fix for the review
// finding where the gate fired unconditionally at the top of
// asset_wildcard_handler / bb_http_host_asset_wildcard, before the
// asset-table lookup, and so denied every unknown probe path along with the
// fallback that was supposed to catch it. Non-vacuity: this test FAILS
// without the fix (the gate check ran before the MISS branch could ever be
// reached, so the fallback stub was never invoked and status stayed 404
// instead of the redirect's 200).
// ===========================================================================

static bool s_fallback_invoked;

static bb_err_t counting_fallback_handler(bb_http_request_t *req)
{
    s_fallback_invoked = true;
    bb_http_resp_set_status(req, 200);
    return BB_OK;
}

void test_bb_http_prov_gate_wire_asset_active_miss_reaches_fallback_ungated(void)
{
    arm_mock_active_fn();
    bb_http_register_assets_cfg_t cfg = { .assets = s_assets, .n = 1, .fallback = counting_fallback_handler };
    bb_http_register_assets(NULL, &cfg);
    s_mock_prov_active = true;
    s_fallback_invoked = false;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    // Unenumerable captive-portal-detection probe path -- never allowlisted,
    // never in the asset table -- must still reach the fallback.
    bb_err_t err = bb_http_host_asset_wildcard(req, "/generate_204");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_fallback_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    s_fallback_invoked = false;
    teardown_wire_test();
}

// ===========================================================================
// Insertion point 4/4 — method_not_allowed_err_handler (host mirror:
// bb_http_host_method_not_allowed). Covers the unmappable-method (HEAD)
// fail-closed branch specifically: `mapped=false` mirrors
// bb_http_method_from_httpd() failing to map an httpd method integer (host
// has no httpd method-int space to fail-map from directly). Non-vacuity:
// the "denied" test below FAILS without the fix (an unmapped method fell
// through to the plain registration check and 405'd a registered route
// instead of 404ing it while provisioning is active).
// ===========================================================================

// A registered-route descriptor for "/ping" -- bb_http_uri_is_registered()
// (route_registry.c) reads the DESCRIBED-route registry, which is separate
// from the imperative bb_http_register_route() dispatch table used by the
// other insertion points in this file; a minimal descriptor is enough since
// only .method/.path are consulted by the 404-vs-405 check under test.
static bb_route_t s_ping_route_descriptor = {
    .method = BB_HTTP_GET, .path = "/ping",
};

void test_bb_http_prov_gate_wire_unmapped_method_active_denies_404(void)
{
    arm_mock_active_fn();
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_ping_route_descriptor));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    // mapped=false simulates HEAD (or any method bb_http_method_from_httpd
    // can't map) against a URI that IS registered (for GET) -- must still
    // fail closed as 404, not leak a 405.
    bb_err_t err = bb_http_host_method_not_allowed(req, false, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    bb_http_route_registry_clear();
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_unmapped_method_inactive_falls_through_to_405(void)
{
    arm_mock_active_fn();
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_ping_route_descriptor));
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    // Provisioning inactive: an unmapped method against a registered URI
    // falls through to the plain registration check, same as pre-PR-3
    // behavior -- registered URI, wrong/unmapped method -> 405.
    bb_err_t err = bb_http_host_method_not_allowed(req, false, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(405, cap.status);

    bb_http_host_capture_free(&cap);
    bb_http_route_registry_clear();
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_unmapped_method_unregistered_uri_404(void)
{
    arm_mock_active_fn();
    bb_http_route_registry_clear();
    TEST_ASSERT_FALSE(s_mock_prov_active);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_method_not_allowed(req, false, BB_HTTP_GET, "/nonexistent");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

// No active-fn registered at all (stays NULL, setUp()'s bb_http_prov_gate_reset()
// already cleared it) -- covers bb_http_prov_gate_is_active()'s NULL-active-fn
// branch specifically, distinct from the armed-but-returns-false case the
// "inactive" test above exercises.
void test_bb_http_prov_gate_wire_unmapped_method_no_active_fn_falls_through_to_405(void)
{
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_ping_route_descriptor));

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_method_not_allowed(req, false, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(405, cap.status);

    bb_http_host_capture_free(&cap);
    bb_http_route_registry_clear();
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_method_not_allowed_null_args_invalid_arg(void)
{
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_method_not_allowed(NULL, true, BB_HTTP_GET, "/ping"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_method_not_allowed(req, true, BB_HTTP_GET, NULL));
}

// mapped=true mirrors bb_http_method_from_httpd() succeeding — this is the
// pre-existing gate check (already covered end-to-end via
// bb_http_host_dispatch_route's own tests above), exercised here too so
// bb_http_host_method_not_allowed's own `if (mapped)` branch (both the
// gate-denies-early-return arm and the gate-allows-falls-through arm) is
// covered directly rather than only through a sibling insertion point.
void test_bb_http_prov_gate_wire_mapped_method_active_non_allowlisted_denies_404(void)
{
    arm_mock_active_fn();
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_ping_route_descriptor));
    // deliberately NOT allowlisted
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_method_not_allowed(req, true, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    bb_http_route_registry_clear();
    teardown_wire_test();
}

void test_bb_http_prov_gate_wire_mapped_method_active_allowlisted_falls_through_to_405(void)
{
    arm_mock_active_fn();
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_ping_route_descriptor));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_method_not_allowed(req, true, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(405, cap.status);

    bb_http_host_capture_free(&cap);
    bb_http_route_registry_clear();
    teardown_wire_test();
}

// ===========================================================================
// Cross-cutting: WP_CLOSING settle window. bb_http_prov_gate has no notion
// of provisioning states (WP_ACTIVE/WP_CLOSING) -- it only ever sees a
// bool from the injected active-fn (see bb_http_prov_gate.h). bb_wifi_prov's
// PR2 signal (bb_wifi_prov_is_active()) already returns true across both
// WP_ACTIVE and WP_CLOSING; this test proves the WIRING makes no separate
// case for "closing" -- whatever the injected fn returns is honored as-is,
// so once bb_wifi_prov_is_active is wired in as the active-fn, the settle
// window gates identically to full-active.
// ===========================================================================

void test_bb_http_prov_gate_wire_closing_settle_window_gates_like_active(void)
{
    arm_mock_active_fn();
    bb_http_register_route(NULL, BB_HTTP_GET, "/ping", counting_stub_handler);
    // Simulates bb_wifi_prov_is_active() during the WP_CLOSING settle window:
    // credentials are already committed but the signal still reads true.
    s_mock_prov_active = true;

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    // /ping is NOT allowlisted here -- during the settle window a
    // non-allowlisted route must still deny, same as full-active.
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}

// ===========================================================================
// No wiring at all (active-fn never set): the seam's documented default —
// bb_http_prov_gate_check() must resolve prov_active to false, so an app
// that never composes provisioning sees zero gating (unchanged behavior).
// ===========================================================================

void test_bb_http_prov_gate_wire_no_active_fn_registered_unaffected(void)
{
    // Deliberately does NOT call arm_mock_active_fn() -- active-fn stays
    // NULL (setUp()'s bb_http_prov_gate_reset() already cleared it).
    bb_http_register_route(NULL, BB_HTTP_GET, "/ping", counting_stub_handler);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_TRUE(s_stub_handler_invoked);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_wire_test();
}
