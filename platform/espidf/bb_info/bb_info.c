#include "bb_info.h"

#include <string.h>

#include "bb_board.h"
#include "bb_wifi.h"
#include "esp_http_server.h"

#define BB_INFO_MAX_EXTENDERS 4

static bb_info_extender_fn s_extenders[BB_INFO_MAX_EXTENDERS];
static int s_extender_count = 0;
static bool s_frozen = false;

bb_err_t bb_info_register_extender(bb_info_extender_fn fn)
{
    if (!fn) return BB_ERR_INVALID_ARG;
    if (s_frozen) return BB_ERR_INVALID_STATE;
    if (s_extender_count >= BB_INFO_MAX_EXTENDERS) return BB_ERR_NO_SPACE;
    s_extenders[s_extender_count++] = fn;
    return BB_OK;
}

static void add_board_fields(cJSON *root, const bb_board_info_t *b)
{
    cJSON_AddStringToObject(root, "board", b->board);
    cJSON_AddStringToObject(root, "project_name", b->project_name);
    cJSON_AddStringToObject(root, "version", b->version);
    cJSON_AddStringToObject(root, "idf_version", b->idf_version);
    cJSON_AddStringToObject(root, "build_date", b->build_date);
    cJSON_AddStringToObject(root, "build_time", b->build_time);
    cJSON_AddStringToObject(root, "chip_model", b->chip_model);
    cJSON_AddNumberToObject(root, "cores", (double)b->cores);
    cJSON_AddStringToObject(root, "mac", b->mac);
    cJSON_AddNumberToObject(root, "flash_size", (double)b->flash_size);
    cJSON_AddNumberToObject(root, "total_heap", (double)b->total_heap);
    cJSON_AddNumberToObject(root, "free_heap", (double)b->free_heap);
    cJSON_AddNumberToObject(root, "app_size", (double)b->app_size);
    cJSON_AddStringToObject(root, "reset_reason", b->reset_reason);
    cJSON_AddBoolToObject(root, "ota_validated", b->ota_validated);
}

static void add_network_object(cJSON *root, const bb_wifi_info_t *w)
{
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             w->bssid[0], w->bssid[1], w->bssid[2],
             w->bssid[3], w->bssid[4], w->bssid[5]);

    cJSON *net = cJSON_CreateObject();
    cJSON_AddStringToObject(net, "ssid", w->ssid);
    cJSON_AddStringToObject(net, "bssid", bssid);
    cJSON_AddNumberToObject(net, "rssi", (double)w->rssi);
    cJSON_AddStringToObject(net, "ip", w->ip);
    cJSON_AddBoolToObject(net, "connected", w->connected);
    cJSON_AddNumberToObject(net, "disc_reason", (double)w->disc_reason);
    cJSON_AddNumberToObject(net, "disc_age_s", (double)w->disc_age_s);
    cJSON_AddNumberToObject(net, "retry_count", (double)w->retry_count);
    cJSON_AddItemToObject(root, "network", net);
}

static esp_err_t info_handler(httpd_req_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    cJSON *root = cJSON_CreateObject();
    add_board_fields(root, &b);
    add_network_object(root, &w);

    for (int i = 0; i < s_extender_count; i++) {
        s_extenders[i](root);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    if (json) cJSON_free(json);
    cJSON_Delete(root);
    return err;
}

esp_err_t bb_info_register_routes(void *server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    s_frozen = true;
    httpd_handle_t h = (httpd_handle_t)server;
    httpd_uri_t uri = {
        .uri = "/api/info", .method = HTTP_GET, .handler = info_handler, .user_ctx = NULL,
    };
    return httpd_register_uri_handler(h, &uri);
}
