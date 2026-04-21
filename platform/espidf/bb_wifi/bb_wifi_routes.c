#include "bb_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", info.ssid);
    cJSON_AddStringToObject(root, "bssid", bssid);
    cJSON_AddNumberToObject(root, "rssi", (double)info.rssi);
    cJSON_AddStringToObject(root, "ip", info.ip);
    cJSON_AddBoolToObject(root, "connected", info.connected);
    cJSON_AddNumberToObject(root, "disc_reason", (double)info.disc_reason);
    cJSON_AddNumberToObject(root, "disc_age_s", (double)info.disc_age_s);
    cJSON_AddNumberToObject(root, "retry_count", (double)info.retry_count);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    if (json) cJSON_free(json);
    cJSON_Delete(root);
    return err;
}

esp_err_t bb_wifi_register_routes(void *server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    httpd_handle_t h = (httpd_handle_t)server;
    httpd_uri_t uri = {
        .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_info_handler, .user_ctx = NULL,
    };
    return httpd_register_uri_handler(h, &uri);
}
