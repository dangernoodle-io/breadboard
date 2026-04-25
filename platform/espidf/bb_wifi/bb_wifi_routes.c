#include "bb_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bb_json.h"
#include "esp_err.h"
#include "esp_http_server.h"

static esp_err_t wifi_info_handler(httpd_req_t *req)
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
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    if (json) bb_json_free_str(json);
    bb_json_free(root);
    return err;
}

bb_err_t bb_wifi_register_routes(void *server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    httpd_handle_t h = (httpd_handle_t)server;
    httpd_uri_t uri = {
        .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_info_handler, .user_ctx = NULL,
    };
    return httpd_register_uri_handler(h, &uri);
}
