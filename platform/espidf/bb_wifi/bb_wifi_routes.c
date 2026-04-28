#include "bb_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bb_http.h"
#include "bb_json.h"

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

    char *json = bb_json_serialize(root);
    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_send(req, json ? json : "{}", json ? strlen(json) : 2);
    if (json) bb_json_free_str(json);
    bb_json_free(root);
    return err;
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

bb_err_t bb_wifi_register_routes(void *server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    return bb_http_register_described_route(server, &s_wifi_route);
}
