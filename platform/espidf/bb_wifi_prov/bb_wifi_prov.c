#include "bb_wifi_prov.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_settings.h"
#include "bb_wifi.h"
#include "wifi_prov_mgr.h"
#include "wifi_prov_policy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "bb_wifi_prov";

// Provisioning state. AP + captive-DNS state moved to bb_wifi_ap (KB 781).
static EventGroupHandle_t s_prov_event_group = NULL;

static bb_wifi_prov_save_cb_t s_save_cb = NULL;

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

    // Parse URL-encoded fields
    char ssid[32] = "", pass[64] = "";
    switch (bb_wifi_prov_parse_body(body, len, ssid, sizeof(ssid), pass, sizeof(pass))) {
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

    if (s_save_cb) {
        bb_err_t cb_err = s_save_cb(req, body, len);
        if (cb_err != BB_OK) return BB_ERR_INVALID_STATE;
    } else {
        bb_http_resp_set_status(req, 204);
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

    // Reserve handler slots for routes registered imperatively below
    // (must happen before ensure_started — once httpd_start runs, the cap
    // is fixed). 1 = POST /save. The captive-portal redirect no longer gets
    // its own GET /* registration — it is folded into the asset wildcard as
    // its no-match fallback (bb_http_register_assets_with_fallback), which
    // is a single "/*" registration already accounted for separately (see
    // ensure_started()'s own comment). The per-asset count (n) is likewise
    // not added: all assets are served via that same single wildcard.
    // 8 = slack for extra() callback routes; consumers needing more must
    // reserve themselves via bb_http_reserve_routes().
    bb_http_reserve_routes(1 + (extra ? 8 : 0));

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
    bb_err_t assets_err = bb_http_register_assets_with_fallback(server, assets, n, prov_redirect_handler);
    if (assets_err != BB_OK) {
        bb_log_e(TAG, "failed to register asset/captive-portal wildcard: %d", (int)assets_err);
        return assets_err;
    }

    bb_log_i(TAG, "provisioning server started on port 80");

    // Prefetch the SSID list so the portal's auto-scan on first page-load
    // returns a populated array instead of an empty cache.
    bb_wifi_scan_start_async();

    return BB_OK;
}

void bb_wifi_prov_stop(void)
{
    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return;

    // Unregister provisioning handlers: /save (POST) and /* (GET catch-all)
    bb_http_unregister_route(server, BB_HTTP_POST, "/save");
    bb_http_unregister_route(server, BB_HTTP_GET, "/*");
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
