#include "bb_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bb_http.h"
#include "bb_json.h"
#include "bb_registry.h"

#include "esp_wifi.h"

static bb_err_t wifi_info_handler(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "ssid", info.ssid);
    bb_json_obj_set_string(root, "bssid", bssid);
    bb_json_obj_set_number(root, "rssi", (double)info.rssi);
    bb_json_obj_set_string(root, "ip", info.ip);
    bb_json_obj_set_bool(root, "connected", info.connected);
    bb_json_obj_set_number(root, "disc_reason", (double)info.disc_reason);
    bb_json_obj_set_number(root, "disc_age_s", (double)info.disc_age_s);
    bb_json_obj_set_number(root, "retry_count", (double)info.retry_count);

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static bb_err_t scan_handler(bb_http_request_t *req)
{
    bb_wifi_scan_start_async();

    bb_wifi_ap_t aps[WIFI_SCAN_MAX];
    memset(aps, 0, sizeof(aps));
    int count = bb_wifi_scan_get_cached(aps, WIFI_SCAN_MAX);

    bb_json_t arr = bb_json_arr_new();
    for (int i = 0; i < count; i++) {
        bb_json_t ap = bb_json_obj_new();
        bb_json_obj_set_string(ap, "ssid", aps[i].ssid);
        bb_json_obj_set_number(ap, "rssi", aps[i].rssi);
        bb_json_obj_set_bool(ap, "secure", aps[i].secure);
        bb_json_arr_append_obj(arr, ap);
    }
    bb_err_t rc = bb_http_resp_send_json(req, arr);
    bb_json_free(arr);
    return rc;
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
    .method   = BB_HTTP_GET,
    .path     = "/api/scan",
    .tag      = "wifi",
    .summary  = "Scan for Wi-Fi networks",
    .responses = s_scan_responses,
    .handler  = scan_handler,
};

static bb_err_t bb_wifi_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t rc;
    rc = bb_http_register_described_route(server, &s_wifi_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_scan_route);
    if (rc != BB_OK) return rc;
    return BB_OK;
}

#if CONFIG_BB_WIFI_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_wifi_routes, bb_wifi_routes_init, 2);
#endif
