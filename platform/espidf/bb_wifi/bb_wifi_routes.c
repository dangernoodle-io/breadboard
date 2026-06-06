#include "bb_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bb_http.h"
#include "bb_json.h"
#include "bb_registry.h"

#include "esp_wifi.h"

#if CONFIG_BB_WIFI_RECONFIGURE
#include "bb_nv_wifi_pending.h"
#endif

static bb_err_t wifi_info_handler(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj, "ssid",        info.ssid);
    bb_http_resp_json_obj_set_str(&obj, "bssid",       bssid);
    bb_http_resp_json_obj_set_int(&obj, "rssi",        (int64_t)info.rssi);
    bb_http_resp_json_obj_set_str(&obj, "ip",          info.ip);
    bb_http_resp_json_obj_set_bool(&obj, "connected",  info.connected);
    bb_http_resp_json_obj_set_int(&obj, "disc_reason", (int64_t)info.disc_reason);
    bb_http_resp_json_obj_set_int(&obj, "disc_age_s",  (int64_t)info.disc_age_s);
    bb_http_resp_json_obj_set_int(&obj, "retry_count", (int64_t)info.retry_count);
    return bb_http_resp_json_obj_end(&obj);
}

static bb_err_t scan_handler(bb_http_request_t *req)
{
    bb_wifi_scan_start_async();

    bb_wifi_ap_t aps[WIFI_SCAN_MAX];
    memset(aps, 0, sizeof(aps));
    int count = bb_wifi_scan_get_cached(aps, WIFI_SCAN_MAX);

    bb_http_json_stream_t stream;
    bb_err_t rc = bb_http_resp_json_arr_begin(req, &stream);
    if (rc != BB_OK) return rc;
    for (int i = 0; i < count; i++) {
        bb_json_t ap = bb_json_obj_new();
        bb_json_obj_set_string(ap, "ssid",   aps[i].ssid);
        bb_json_obj_set_number(ap, "rssi",   aps[i].rssi);
        bb_json_obj_set_bool(ap,   "secure", aps[i].secure);
        bb_http_resp_json_arr_emit(&stream, ap);
        bb_json_free(ap);
    }
    return bb_http_resp_json_arr_end(&stream);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_wifi_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"ssid\":{\"type\":\"string\"},"
      "\"bssid\":{\"type\":\"string\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"ip\":{\"type\":\"string\"},"
      "\"connected\":{\"type\":\"boolean\"},"
      "\"disc_reason\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"retry_count\":{\"type\":\"integer\"}},"
      "\"required\":[\"ssid\",\"connected\"]}",
      "current Wi-Fi connection info" },
    { 0 },
};

static const bb_route_t s_wifi_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/wifi",
    .tag      = "wifi",
    .summary  = "Get Wi-Fi connection info",
    .responses = s_wifi_responses,
    .handler  = wifi_info_handler,
};

static const bb_route_response_t s_scan_responses[] = {
    { 200, "application/json",
      "{\"type\":\"array\","
      "\"items\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"ssid\":{\"type\":\"string\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"secure\":{\"type\":\"boolean\"}},"
      "\"required\":[\"ssid\",\"rssi\",\"secure\"]}}",
      "list of visible access points" },
    { 0 },
};

static const bb_route_t s_scan_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/scan",
    .tag      = "wifi",
    .summary  = "Trigger Wi-Fi network scan and return cached results",
    .responses = s_scan_responses,
    .handler  = scan_handler,
};

#if CONFIG_BB_WIFI_RECONFIGURE

static bb_err_t wifi_patch_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing or oversized body");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "read failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid JSON");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    char ssid[BB_WIFI_PENDING_SSID_MAX + 1];
    char pass[BB_WIFI_PENDING_PASS_MAX + 1];
    ssid[0] = '\0';
    pass[0] = '\0';
    bb_json_obj_get_string(parsed, "ssid",     ssid, sizeof(ssid));
    bb_json_obj_get_string(parsed, "password", pass, sizeof(pass));
    bb_json_free(parsed);

    /* Determine a descriptive validation error before calling validate. */
    const char *err_msg = NULL;
    if (!ssid[0]) {
        err_msg = "ssid required";
    } else if (strlen(ssid) > BB_WIFI_PENDING_SSID_MAX) {
        err_msg = "ssid too long";
    } else if (strlen(pass) > BB_WIFI_PENDING_PASS_MAX) {
        err_msg = "password too long";
    }

    if (!err_msg && bb_wifi_pending_validate(ssid, pass) != BB_OK) {
        err_msg = "invalid credentials";
    }

    if (err_msg) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", err_msg);
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting_to_try_wifi");
    bb_http_resp_json_obj_end(&obj);

    bb_wifi_reconfigure(ssid, pass);
    return BB_OK;
}

static const bb_route_response_t s_wifi_patch_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "reconfigure accepted; device will reboot to try new credentials" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "invalid request body or credentials" },
    { 0 },
};

static const bb_route_t s_wifi_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/wifi",
    .tag                  = "wifi",
    .summary              = "Stage new Wi-Fi credentials and arm deferred reboot",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\","
                            "\"properties\":{\"ssid\":{\"type\":\"string\",\"maxLength\":31},"
                            "\"password\":{\"type\":\"string\",\"maxLength\":63}},"
                            "\"required\":[\"ssid\"]}",
    .responses            = s_wifi_patch_responses,
    .handler              = wifi_patch_handler,
};

#endif /* CONFIG_BB_WIFI_RECONFIGURE */

static bb_err_t bb_wifi_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t rc;
    rc = bb_http_register_described_route(server, &s_wifi_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_scan_route);
    if (rc != BB_OK) return rc;
#if CONFIG_BB_WIFI_RECONFIGURE
    rc = bb_http_register_described_route(server, &s_wifi_patch_route);
    if (rc != BB_OK) return rc;
#endif
    return BB_OK;
}

#if CONFIG_BB_WIFI_ROUTES_AUTOREGISTER
static bb_err_t bb_wifi_routes_reserve(void)
{
#if CONFIG_BB_WIFI_RECONFIGURE
    bb_http_reserve_routes(3);  // GET /api/wifi + POST /api/scan + PATCH /api/wifi
#else
    bb_http_reserve_routes(2);  // GET /api/wifi + POST /api/scan
#endif
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_wifi_routes, bb_wifi_routes_reserve);
BB_REGISTRY_REGISTER_N(bb_wifi_routes, bb_wifi_routes_init, 2);
#endif
