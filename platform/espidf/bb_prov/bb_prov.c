#include "bb_prov.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_settings.h"
#include "bb_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "bb_prov";

// Provisioning state. AP + captive-DNS state moved to bb_wifi_ap (KB 781).
static EventGroupHandle_t s_prov_event_group = NULL;

static bb_prov_save_cb_t s_save_cb = NULL;

void bb_prov_set_save_callback(bb_prov_save_cb_t cb) { s_save_cb = cb; }

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
    switch (bb_prov_parse_body(body, len, ssid, sizeof(ssid), pass, sizeof(pass))) {
        case BB_PROV_PARSE_EMPTY_BODY: {
            bb_http_resp_set_status(req, 400);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "Empty body");
            bb_http_resp_json_obj_end(&obj);
            return BB_ERR_INVALID_ARG;
        }
        case BB_PROV_PARSE_SSID_REQUIRED: {
            bb_http_resp_set_status(req, 400);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "SSID required");
            bb_http_resp_json_obj_end(&obj);
            return BB_ERR_INVALID_ARG;
        }
        case BB_PROV_PARSE_OK:
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

    bb_prov_signal_done();
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
// bb_prov_start_ap()/bb_prov_stop_ap()/dns_task() are now
// bb_wifi_ap_start()/bb_wifi_ap_stop() (components/bb_wifi_ap). bb_prov
// composes bb_wifi_ap as a lifecycle step; it no longer owns the AP or the
// DNS responder.

bool bb_prov_wait_done(uint32_t timeout_ms)
{
    // s_prov_event_group is created deterministically in bb_prov_start(),
    // before the /save route is registered -- so it always exists by the
    // time any caller can reach here (bb_prov_start() must precede a wait).
    // NULL means bb_prov_start() was never called: no route can signal, so
    // there is nothing to wait for -- treat like a timeout.
    if (s_prov_event_group == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_prov_event_group, PROV_DONE_BIT, pdTRUE, pdTRUE, ticks);
    return (bits & PROV_DONE_BIT) != 0;
}

void bb_prov_signal_done(void)
{
    // See bb_prov_wait_done(): the group is created once in bb_prov_start().
    // NULL here means bb_prov_start() was never called -- no-op.
    if (s_prov_event_group != NULL) {
        xEventGroupSetBits(s_prov_event_group, PROV_DONE_BIT);
    }
}

// bb_prov_start() reserves route slots, starts the shared HTTP server, and
// registers /save + caller assets + a captive-portal GET /* wildcard. Route
// composition for the rest of the app (system/wifi/info/etc.) is the
// consumer's responsibility via codegen/handwire BEFORE calling this — the
// old bb_init registry walker that used to drive it here is gone (DI
// demolition; codegen + handwire are the only composition paths).
bb_err_t bb_prov_start(const bb_http_asset_t *assets, size_t n,
                       bb_prov_extra_routes_fn_t extra)
{
    // Create the provisioning-done event group deterministically, BEFORE
    // /save is registered below -- the handler runs on the HTTP task and
    // bb_prov_wait_done() on the app task, so creating it lazily on either
    // side races: a /save landing before the app task's first wait_done()
    // call would create a *second* group there, leaking the first and
    // blocking the waiter forever (lost wakeup). Idempotent: harmless if
    // bb_prov_start() is ever called more than once.
    if (s_prov_event_group == NULL) {
        s_prov_event_group = xEventGroupCreate();
        if (s_prov_event_group == NULL) return BB_ERR_INVALID_STATE;
    }

    // Reserve handler slots for routes registered imperatively below
    // (must happen before ensure_started — once httpd_start runs, the cap
    // is fixed). 2 = POST /save + GET /* captive wildcard. The per-asset count
    // (n) is no longer added: assets are served via a single GET /* wildcard
    // registered unconditionally, already accounted for in
    // BB_HTTP_EXPLICIT_ASSET_WILDCARD inside ensure_started().
    // 8 = slack for extra() callback routes; consumers needing more must
    // reserve themselves via bb_http_reserve_routes().
    bb_http_reserve_routes(2 + (extra ? 8 : 0));

    // Ensure the shared HTTP server is started (internal helper)
    bb_err_t err = bb_http_server_ensure_started();
    if (err != BB_OK) return err;

    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return BB_ERR_INVALID_STATE;

    bb_http_register_route(server, BB_HTTP_POST, "/save", prov_save_handler);

    // Register consumer assets (caller MUST supply at least one asset with path="/")
    if (assets && n > 0) {
        bb_http_register_assets(server, assets, n);
    }

    // Consumer's dynamic endpoints (e.g. advanced-UI backing routes).
    if (extra) {
        bb_err_t rc = extra(server);
        if (rc != BB_OK) return rc;
    }

    // Captive-portal wildcard LAST so all specific GETs win first-match.
    bb_http_register_route(server, BB_HTTP_GET, "/*", prov_redirect_handler);

    bb_log_i(TAG, "provisioning server started on port 80");

    // Prefetch the SSID list so the portal's auto-scan on first page-load
    // returns a populated array instead of an empty cache.
    bb_wifi_scan_start_async();

    return BB_OK;
}

void bb_prov_stop(void)
{
    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return;

    // Unregister provisioning handlers: /save (POST) and /* (GET catch-all)
    bb_http_unregister_route(server, BB_HTTP_POST, "/save");
    bb_http_unregister_route(server, BB_HTTP_GET, "/*");
}
