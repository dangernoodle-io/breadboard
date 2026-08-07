#include "bb_wifi_prov.h"
#include "bb_http_prov_gate.h"
#include "bb_http_serialize_stream.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_settings.h"
#include "bb_wifi.h"
#include "bb_wifi_prov_scan_wire.h"
#include "wifi_prov_gate_wire.h"
#include "wifi_prov_mgr.h"
#include "wifi_prov_policy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "bb_wifi_prov";

// Provisioning state. AP + captive-DNS state moved to bb_wifi_ap (KB 781).
static EventGroupHandle_t s_prov_event_group = NULL;

static bb_wifi_prov_save_cb_t s_save_cb = NULL;

// B1-809 portal: true once bb_wifi_prov has triggered at least one WiFi
// scan on this portal instance (the bb_wifi_prov_start() prefetch below, or
// a "refresh"-flagged POST /api/wifi/scan). Feeds
// bb_wifi_prov_scan_classify_state()'s NOT_STARTED/PENDING/READY tri-state
// (bb_wifi_prov_scan_wire.h). Only ever set true, never reset -- a portal
// instance that has scanned once stays "has scanned" for its lifetime, so a
// racy concurrent write from the HTTP task and this file's own prefetch
// call is a harmless idempotent write, same posture as bb_wifi.c's own
// unlocked s_scan_in_progress flag.
static bool s_scan_ever_requested = false;

// B1-1279 PR-4: guards bb_http_prov_gate_set_active_fn()/wifi_prov_gate_wire_
// register() so a second bb_wifi_prov_start() call (documented idempotent,
// see this function's own comment) does not re-add duplicate allowlist
// entries and burn BB_HTTP_PROV_ALLOWLIST_CAP slots. Set exactly once, from
// the HTTP task inside bb_wifi_prov_start() -- same single-writer posture as
// s_scan_ever_requested above.
static bool s_gate_wired = false;

void bb_wifi_prov_set_save_callback(bb_wifi_prov_save_cb_t cb) { s_save_cb = cb; }

#define PROV_DONE_BIT BIT0

static void set_common_headers(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Connection", "close");
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");
}

// Handle provisioning form submission
static bb_err_t prov_save_handler(bb_http_request_t *req)
{
    set_common_headers(req);
    char body[512];

    // Validate content length to prevent silent body truncation
    int content_len = bb_http_req_body_len(req);
    if (content_len > (int)(sizeof(body) - 1)) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "Body too large");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }
    int len = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "Empty body");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }
    body[len] = '\0';

    // Parse URL-encoded fields. hostname is OPTIONAL (see
    // bb_wifi_prov_parse_body's doc) -- 512-byte body[] above already has
    // ample headroom for this one extra short field on top of ssid/pass.
    char ssid[32] = "", pass[64] = "", hostname[33] = "";
    switch (bb_wifi_prov_parse_body(body, len, ssid, sizeof(ssid), pass, sizeof(pass),
                                     hostname, sizeof(hostname))) {
        case BB_WIFI_PROV_PARSE_EMPTY_BODY: {
            bb_http_resp_set_status(req, 400);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "Empty body");
            bb_http_resp_json_obj_end(&obj);
            return BB_ERR_INVALID_ARG;
        }
        case BB_WIFI_PROV_PARSE_SSID_REQUIRED: {
            bb_http_resp_set_status(req, 400);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "SSID required");
            bb_http_resp_json_obj_end(&obj);
            return BB_ERR_INVALID_ARG;
        }
        case BB_WIFI_PROV_PARSE_OK:
            break;
    }

    // Routes through bb_settings' single atomic commit rather than bb_nv's
    // now-deleted two-key sequential write (B1-750, bb_nv dissolution epic
    // B1-708) -- byte-compat with the prior bb_nv_config_set_wifi: same NVS
    // namespace ("bb_cfg") and keys ("wifi_ssid"/"wifi_pass"), same
    // nvs_set_str encoding, so a provisioned board's existing creds are
    // unaffected by this switch.
    bb_err_t err = bb_settings_wifi_set(ssid, pass);
    if (err != BB_OK) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "Failed to save config");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }

    // Hostname is a SEPARATE, deliberately NON-atomic single-key write from
    // the creds commit above -- bb_settings_wifi_set() routes through
    // bb_settings' staged/atomic commit (see the comment above it), while
    // this is a plain single bb_settings_hostname_set() call. A half-write
    // (e.g. power loss between the two) leaves creds saved + the prior/
    // default hostname -- recoverable and non-stranding, unlike a failure
    // here blocking the creds save above ever would be.
    //
    // Absent/empty hostname is not an error (optional field). An invalid
    // (but non-empty) hostname is logged and DISCARDED rather than failing
    // the request: bb_settings_hostname_set() already owns RFC-1123
    // validation, and a 400 here would strand the user with no creds saved
    // even though the creds write above already succeeded -- creds are the
    // important part of this form, so a bad hostname degrades to "hostname
    // unchanged", not "provisioning failed".
    bool hostname_saved = false;
    if (hostname[0] != '\0') {
        bb_err_t host_err = bb_settings_hostname_set(hostname);
        if (host_err != BB_OK) {
            bb_log_w(TAG, "invalid hostname %s, keeping existing value", hostname);
        } else {
            hostname_saved = true;
        }
    }

    if (s_save_cb) {
        bb_err_t cb_err = s_save_cb(req, body, len);
        if (cb_err != BB_OK) return BB_ERR_INVALID_STATE;
    } else {
        // No consumer callback: render a minimal HTML confirmation instead
        // of 204 No Content. A 204 leaves the browser nothing to render, so
        // moments later this AP tears down mid-response and the user sees a
        // browser error and assumes the save failed -- it did not (HW-
        // confirmed). ssid/hostname are attacker-controlled in the general
        // case (whatever the client just POSTed), so
        // bb_wifi_prov_render_saved_page() HTML-escapes both internally.
        //
        // MEDIUM review fix: pass the hostname through
        // bb_wifi_prov_saved_page_hostname() so an INVALID hostname (rejected
        // above, prior value kept) is reported as "" here rather than as a
        // find-it-at-X.local claim for an X that was never saved.
        char page[BB_WIFI_PROV_SAVED_PAGE_MAX];
        const char *page_hostname = bb_wifi_prov_saved_page_hostname(hostname, hostname_saved);
        size_t page_len = bb_wifi_prov_render_saved_page(page, sizeof(page), ssid, page_hostname);
        bb_http_resp_set_type(req, "text/html");
        bb_http_resp_send_chunk(req, page, (int)page_len);
        bb_http_resp_send_chunk(req, NULL, 0);
    }

    bb_wifi_prov_signal_done();

    // Notify the provisioning manager FSM (B1-809 PR3b) LAST, after
    // signal_done() -- the HTTP response above is fully written/queued
    // before the manager can begin WP_CLOSING teardown (see the "WHY THE
    // SETTLE STATE" block in wifi_prov_policy.c). No-op (queue unset) if no
    // manager is running -- inert until PR4 wires wifi_prov_mgr_init().
    wifi_prov_mgr_on_save();

    return BB_OK;
}

static bb_err_t prov_redirect_handler(bb_http_request_t *req)
{
    set_common_headers(req);
    bb_http_resp_set_status(req, 302);
    bb_http_resp_set_header(req, "Location", "http://192.168.4.1/");
    bb_http_resp_send_chunk(req, NULL, 0);
    return BB_OK;
}

// POST /api/wifi/scan -- bb_wifi_prov's OWN scan endpoint (B1-809 portal).
// Does not require bb_wifi_http: the portal's captive form must get an SSID
// list even in a bb_wifi_prov-only composition. See coexistence handling in
// bb_wifi_prov_start() below and bb_wifi_prov_scan_wire.h for the response
// shape/state-machine rationale.
//
// Serves the CACHE by default rather than mirroring bb_wifi_http_scan_
// wire_fill()'s trigger-then-immediately-read defect (that always returns
// an empty list on the first call, since the scan it just kicked off
// hasn't finished). An explicit refresh is opt-in via the "refresh" query
// param (bare `?refresh` or `?refresh=1` -- bb_http_req_query_has_key()
// detects either form): the caller triggers a background scan and polls
// this same route again shortly for fresh results, same pattern the
// existing form's auto-scan-on-load already relies on via
// bb_wifi_prov_start()'s own prefetch below.
//
// CAUTION for "refresh" callers: the scan it triggers hops the radio off
// the SoftAP channel, which transiently drops SoftAP-associated clients --
// including the phone/laptop that just issued this request. In practice
// the HTTP response below is written and returned before that disruption,
// because bb_wifi_scan_start_async() only spawns a low-priority detached
// task and returns immediately -- but that ordering is scheduler-
// cooperative, not fence-guaranteed, so don't depend on it. A portal UI
// using "refresh" should expect its own connection to drop and retry/
// reconnect rather than treat a failed response as an error. See
// bb_wifi_prov.h's route doc and bb_wifi_prov_scan_wire.h for the same
// note at the public-facing doc surfaces.
static bb_err_t prov_scan_handler(bb_http_request_t *req)
{
    set_common_headers(req);

    if (bb_http_req_query_has_key(req, "refresh")) {
        bb_wifi_scan_start_async();
        s_scan_ever_requested = true;
    }

    // Non-blocking (0ms timeout) -- see bb_wifi_scan_wait_idle()'s own doc
    // comment (bb_wifi.h). Probed BEFORE the cache read below -- deliberately
    // the opposite order from "most current data first". If an in-flight
    // scan's cache write lands in the gap between these two calls, reading
    // idle first means we still observe idle=false and report "pending",
    // even though "aps" (read a moment later) may already hold the fresher
    // result. That under-reports freshness, which is harmless: the caller
    // just polls again. The reverse order (cache first, idle second) would
    // let that same race land the completion in the OTHER gap, making idle
    // read true while "aps" still holds the PREVIOUS scan -- reporting
    // "ready" with stale rows presented as authoritative, which callers
    // cannot detect. Do not swap this back to "optimize" freshness.
    bool idle = bb_wifi_scan_wait_idle(0);
    bb_wifi_ap_t aps[WIFI_SCAN_MAX];
    int count = bb_wifi_scan_get_cached(aps, WIFI_SCAN_MAX);
    bb_wifi_prov_scan_state_t state =
        bb_wifi_prov_scan_classify_state(s_scan_ever_requested, idle);

    bb_wifi_prov_scan_wire_t snap;
    bb_wifi_prov_scan_wire_fill(&snap, aps, count, state);

    return bb_http_serialize_stream(req, &bb_wifi_prov_scan_wire_desc, &snap);
}

// AP + captive DNS bring-up moved to bb_wifi_ap (KB 781, PR2) --
// bb_wifi_prov_start_ap()/bb_wifi_prov_stop_ap()/dns_task() are now
// bb_wifi_ap_start()/bb_wifi_ap_stop() (components/bb_wifi_ap). bb_wifi_prov
// composes bb_wifi_ap as a lifecycle step; it no longer owns the AP or the
// DNS responder.

bool bb_wifi_prov_wait_done(uint32_t timeout_ms)
{
    // s_prov_event_group is created deterministically in bb_wifi_prov_start(),
    // before the /save route is registered -- so it always exists by the
    // time any caller can reach here (bb_wifi_prov_start() must precede a wait).
    // NULL means bb_wifi_prov_start() was never called: no route can signal, so
    // there is nothing to wait for -- treat like a timeout.
    if (s_prov_event_group == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_prov_event_group, PROV_DONE_BIT, pdTRUE, pdTRUE, ticks);
    return (bits & PROV_DONE_BIT) != 0;
}

void bb_wifi_prov_signal_done(void)
{
    // See bb_wifi_prov_wait_done(): the group is created once in bb_wifi_prov_start().
    // NULL here means bb_wifi_prov_start() was never called -- no-op.
    if (s_prov_event_group != NULL) {
        xEventGroupSetBits(s_prov_event_group, PROV_DONE_BIT);
    }
}

// bb_wifi_prov_start() reserves route slots, starts the shared HTTP server, and
// registers /save + caller assets + a captive-portal GET /* wildcard. Route
// composition for the rest of the app (system/wifi/info/etc.) is the
// consumer's responsibility via codegen/handwire BEFORE calling this — the
// old bb_init registry walker that used to drive it here is gone (DI
// demolition; codegen + handwire are the only composition paths).
//
// This function is ESP-IDF-only (not in the host test target); its
// registration-order and error-propagation branches (save_err/assets_err/
// extra()-before-wildcard) are validated on hardware, not by host tests.
bb_err_t bb_wifi_prov_start(const bb_http_asset_t *assets, size_t n,
                       bb_wifi_prov_extra_routes_fn_t extra)
{
    // Create the provisioning-done event group deterministically, BEFORE
    // /save is registered below -- the handler runs on the HTTP task and
    // bb_wifi_prov_wait_done() on the app task, so creating it lazily on either
    // side races: a /save landing before the app task's first wait_done()
    // call would create a *second* group there, leaking the first and
    // blocking the waiter forever (lost wakeup). Idempotent: harmless if
    // bb_wifi_prov_start() is ever called more than once.
    if (s_prov_event_group == NULL) {
        s_prov_event_group = xEventGroupCreate();
        if (s_prov_event_group == NULL) return BB_ERR_INVALID_STATE;
    }

    // Ensure the shared HTTP server is started (internal helper)
    bb_err_t err = bb_http_server_ensure_started();
    if (err != BB_OK) return err;

    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return BB_ERR_INVALID_STATE;

    bb_err_t save_err = bb_http_register_route(server, BB_HTTP_POST, "/save", prov_save_handler);
    if (save_err != BB_OK) {
        bb_log_e(TAG, "failed to register POST /save: %d", (int)save_err);
        return save_err;
    }

    // POST /api/wifi/scan -- registered here, right after /save and BEFORE
    // extra()/the asset wildcard, for two reasons:
    //  1. /api/* paths are served through bb_dispatch_api's own flat
    //     (method,path) table (route_registry.c), NOT esp_http_server's
    //     httpd handler slots -- unlike /save or the GET /* wildcard below,
    //     this registration's position relative to THOSE is irrelevant to
    //     request routing.
    //  2. Registration order DOES matter within the dispatch table itself:
    //     it is first-registration-wins (bb_dispatch_api_add). Registering
    //     bb_wifi_prov's own handler before calling extra() means a
    //     consumer's extra() callback can never accidentally shadow this
    //     component's endpoint within a single bb_wifi_prov_start() call --
    //     bb_wifi_prov OWNS this path in its own composition, even though
    //     it tolerates LOSING the race to a separately-composed bb_wifi_http
    //     (see the dup handling below).
    //
    // bb_http_register_route(), for a "/api/" path, delegates to
    // bb_dispatch_api_add() -- see platform/espidf/bb_http_server/bb_http.c.
    // A duplicate (method,path) there logs a warning and returns
    // BB_ERR_INVALID_STATE (first registration wins) UNLESS
    // CONFIG_BB_HTTP_ROUTE_DUP_STRICT is enabled, in which case
    // bb_dispatch_api_add() itself asserts(0) before ever returning --
    // that path is NOT interceptable here (default off; do not enable it
    // in a composition that mixes bb_wifi_prov and bb_wifi_http, both of
    // which register this same route). bb_http_register_route() itself
    // still swallows a non-strict duplicate to BB_OK for any "/api/" path
    // (B1-1253 -- see its own comment: this is the deliberately-tolerated
    // case, distinct from a genuine registration failure, which now
    // propagates instead of being swallowed) -- so the BB_ERR_INVALID_STATE
    // branch below is defensive/currently-dead, not the live path a
    // duplicate takes today; kept in case that swallowing behavior is ever
    // tightened at this call site specifically (e.g. by calling
    // bb_dispatch_api_add directly instead of bb_http_register_route).
    bb_err_t scan_err = bb_http_register_route(server, BB_HTTP_POST, "/api/wifi/scan", prov_scan_handler);
    if (scan_err == BB_ERR_INVALID_STATE) {
        bb_log_i(TAG, "POST /api/wifi/scan already served by another component");
    } else if (scan_err != BB_OK) {
        bb_log_e(TAG, "failed to register POST /api/wifi/scan: %d", (int)scan_err);
        return scan_err;
    }

    // Consumer's dynamic endpoints (e.g. advanced-UI backing routes) — must be
    // registered BEFORE the asset/fallback wildcard below so their specific
    // routes win esp_http_server's first-registered-wins wildcard matching.
    if (extra) {
        bb_err_t rc = extra(server);
        if (rc != BB_OK) {
            bb_log_e(TAG, "extra route registration failed: %d", (int)rc);
            return rc;
        }
    }

    // Register consumer assets (caller MUST supply at least one asset with
    // path="/"; assets may be NULL only when n == 0, e.g. a portal with no
    // caller-supplied UI) plus the captive-portal redirect as the wildcard's
    // no-match fallback. This is ONE "/*" registration instead of a second,
    // doomed GET /* registration — ESP-IDF allows only one handler per
    // method+path, so a separate captive wildcard registered after the asset
    // wildcard is silently rejected (ESP_ERR_HTTPD_HANDLER_EXISTS) and the
    // redirect never fires. Registered LAST so all specific GETs above win
    // first-match.
    // INVARIANT: prov_redirect_handler is dispatched ungated on an asset
    // MISS (bb_http.c's asset_wildcard_handler, "deliberately NOT gated").
    // That's safe only because reaching this line already means THIS call
    // won the single "/*" httpd registration below — wifi_prov_policy.c's
    // wp_active_on_entry (which flips the gate active) runs only after
    // act_try_prov_start observes that success. Do not pass a non-NULL
    // fallback to bb_http_register_assets() from anywhere that can be
    // reached while the gate is active without re-checking that ordering
    // (see that function's own warning).
    bb_http_register_assets_cfg_t assets_cfg = {
        .assets   = assets,
        .n        = n,
        .fallback = prov_redirect_handler,
    };
    bb_err_t assets_err = bb_http_register_assets(server, &assets_cfg);
    if (assets_err != BB_OK) {
        bb_log_e(TAG, "failed to register asset/captive-portal wildcard: %d", (int)assets_err);
        return assets_err;
    }

    // B1-1279 PR-4: activate the provisioning route gate. Wires bb_wifi_prov_
    // is_active() as bb_http_prov_gate's signal source and allowlists exactly
    // the routes provisioning cannot complete without (see wifi_prov_gate_
    // wire.h) -- everything else served by the shared HTTP server 404s while
    // WP_ACTIVE/WP_CLOSING (bb_http_prov_gate_check()'s call sites in
    // platform/espidf/bb_http_server/bb_http.c). Guarded by s_gate_wired so a
    // second bb_wifi_prov_start() call doesn't double-register allowlist
    // entries. Runs BEFORE this function returns BB_OK, which is always
    // before wp_active_on_entry can flip the gate active (that hook fires
    // only after this call's caller, act_try_prov_start, observes BB_OK --
    // see wifi_prov_policy.c) -- so the gate is never active with an
    // incomplete allowlist.
    if (!s_gate_wired) {
        bb_http_prov_gate_set_active_fn(bb_wifi_prov_is_active);
        bb_err_t gate_err = wifi_prov_gate_wire_register(assets, n);
        if (gate_err != BB_OK) {
            // MEDIUM review fix (B1-1279 PR-4): this used to be logged and
            // discarded, so bb_wifi_prov_start() still returned BB_OK with no
            // other trace of a degraded portal. Deliberately still return
            // BB_OK here rather than propagating gate_err out of this
            // function -- a composition whose fixed(3)+n allowlist entries
            // exceed BB_HTTP_PROV_ALLOWLIST_CAP is a build-time asset-count
            // mistake, not a security regression (the gate still fails
            // CLOSED on whatever didn't fit), and failing the whole
            // provisioning flow over it would leave the device
            // unprovisionable rather than merely portal-degraded. Instead,
            // wifi_prov_gate_wire_register() itself records the incomplete
            // outcome (wifi_prov_gate_wire_last_registration_complete()) so
            // bb_wifi_prov_gate_degraded() can surface it distinctly to any
            // caller that wants to detect and report a degraded portal
            // rather than relying on this one log line.
            bb_log_e(TAG, "provisioning allowlist registration incomplete: %d "
                     "(BB_HTTP_PROV_ALLOWLIST_CAP too small for this composition?) "
                     "-- portal degraded, see bb_wifi_prov_gate_degraded()",
                     (int)gate_err);
        }
        s_gate_wired = true;
    }

    bb_log_i(TAG, "provisioning server started on port 80");

    // Prefetch the SSID list so the portal's auto-scan on first page-load
    // returns a populated array instead of an empty cache. Marks the scan
    // as "ever requested" (bb_wifi_prov_scan_classify_state()) so a
    // page-load hitting POST /api/wifi/scan before this prefetch completes
    // sees PENDING, not NOT_STARTED.
    bb_wifi_scan_start_async();
    s_scan_ever_requested = true;

    return BB_OK;
}

void bb_wifi_prov_stop(void)
{
    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return;

    // Unregister provisioning handlers: /save (POST) and /* (GET catch-all)
    bb_http_unregister_route(server, BB_HTTP_POST, "/save");
    bb_http_unregister_route(server, BB_HTTP_GET, "/*");

    // POST /api/wifi/scan is deliberately left registered: bb_http_unregister_
    // route() calls httpd_unregister_uri_handler(), which only knows about
    // real httpd handler slots -- /api/* entries live in bb_dispatch_api's
    // separate flat table (route_registry.c) instead, and that table exposes
    // no single-entry removal, only bb_dispatch_api_reset() (whole-table,
    // test-only). Same posture bb_wifi_http's own /api/wifi/scan takes;
    // harmless to leave answering after the portal closes.
}

// B1-809 PR4 — boot-time / user-requested provisioning entry. LANDS INERT:
// no in-tree caller yet (see this component's README / commit for the
// inertness note); a future consumer wires bb_wifi_prov_autoinit() into its
// own boot sequence (codegen or handwire) once it's ready to compose the
// provisioning portal.

bb_err_t bb_wifi_prov_autoinit(const bb_http_asset_t *assets, size_t n,
                                bb_wifi_prov_extra_routes_fn_t extra)
{
    bool has_creds = bb_settings_wifi_has_creds();
    bool provisioned = bb_settings_wifi_provisioned_get();

    wp_event_t entry_event;
    if (!wifi_prov_entry_decision(has_creds, provisioned, &entry_event)) {
        // Creds already present -- no portal, ever (see
        // wifi_prov_entry_decision's doc). Deliberately does NOT start the
        // manager task here -- nothing needs it (see this function's own
        // doc in bb_wifi_prov.h for the documented request_portal tradeoff).
        return BB_OK;
    }

    bb_err_t err = wifi_prov_mgr_init(assets, n, extra);
    if (err != BB_OK) return err;

    wifi_prov_mgr_post_entry(entry_event);
    return BB_OK;
}

void bb_wifi_prov_request_portal(void)
{
    wifi_prov_mgr_post_entry(EV_ENTRY_USER_REQUESTED);
}

bool bb_wifi_prov_is_active(void)
{
    return wifi_prov_mgr_is_active();
}

bool bb_wifi_prov_gate_degraded(void)
{
    return !wifi_prov_gate_wire_last_registration_complete();
}
