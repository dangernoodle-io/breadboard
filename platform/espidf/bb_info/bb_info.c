#include "bb_info.h"

#include <string.h>

#include "bb_board.h"
#include "bb_json.h"
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

static void add_board_fields(bb_json_t root, const bb_board_info_t *b)
{
    bb_json_obj_set_string(root, "board", b->board);
    bb_json_obj_set_string(root, "project_name", b->project_name);
    bb_json_obj_set_string(root, "version", b->version);
    bb_json_obj_set_string(root, "idf_version", b->idf_version);
    bb_json_obj_set_string(root, "build_date", b->build_date);
    bb_json_obj_set_string(root, "build_time", b->build_time);
    bb_json_obj_set_string(root, "chip_model", b->chip_model);
    bb_json_obj_set_number(root, "cores", (double)b->cores);
    bb_json_obj_set_string(root, "mac", b->mac);
    bb_json_obj_set_number(root, "flash_size", (double)b->flash_size);
    bb_json_obj_set_number(root, "total_heap", (double)b->total_heap);
    bb_json_obj_set_number(root, "free_heap", (double)b->free_heap);
    bb_json_obj_set_number(root, "app_size", (double)b->app_size);
    bb_json_obj_set_string(root, "reset_reason", b->reset_reason);
    bb_json_obj_set_bool(root, "ota_validated", b->ota_validated);
}

static void add_network_object(bb_json_t root, const bb_wifi_info_t *w)
{
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             w->bssid[0], w->bssid[1], w->bssid[2],
             w->bssid[3], w->bssid[4], w->bssid[5]);

    bb_json_t net = bb_json_obj_new();
    bb_json_obj_set_string(net, "ssid", w->ssid);
    bb_json_obj_set_string(net, "bssid", bssid);
    bb_json_obj_set_number(net, "rssi", (double)w->rssi);
    bb_json_obj_set_string(net, "ip", w->ip);
    bb_json_obj_set_bool(net, "connected", w->connected);
    bb_json_obj_set_number(net, "disc_reason", (double)w->disc_reason);
    bb_json_obj_set_number(net, "disc_age_s", (double)w->disc_age_s);
    bb_json_obj_set_number(net, "retry_count", (double)w->retry_count);
    bb_json_obj_set_obj(root, "network", net);
}

static esp_err_t info_handler(httpd_req_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    bb_json_t root = bb_json_obj_new();
    add_board_fields(root, &b);
    add_network_object(root, &w);

    for (int i = 0; i < s_extender_count; i++) {
        s_extenders[i](root);
    }

    char *json = bb_json_serialize(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    if (json) bb_json_free_str(json);
    bb_json_free(root);
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
