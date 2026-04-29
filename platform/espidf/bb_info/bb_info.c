#include "bb_info.h"

#include <string.h>

#include "bb_board.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_wifi.h"

#define BB_INFO_MAX_EXTENDERS 4

static const char *TAG = "bb_info";

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
    bb_json_obj_set_number(root, "heap_free_total", (double)bb_board_heap_free_total());
    bb_json_obj_set_number(root, "heap_free_internal", (double)bb_board_heap_free_internal());
    bb_json_obj_set_number(root, "heap_minimum_ever", (double)bb_board_heap_minimum_ever());
    bb_json_obj_set_number(root, "chip_revision", (double)bb_board_chip_revision());
    bb_json_obj_set_number(root, "cpu_freq_mhz", (double)bb_board_cpu_freq_mhz());
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

static bb_err_t info_handler(bb_http_request_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    bb_json_t root = bb_json_obj_new();
    add_board_fields(root, &b);
    add_network_object(root, &w);

    // Add HTTP handler telemetry
    extern size_t bb_http_route_handler_count(void);
    extern size_t bb_http_route_handler_cap(void);
    bb_json_obj_set_number(root, "http_handler_count", (double)bb_http_route_handler_count());
    bb_json_obj_set_number(root, "http_handler_cap", (double)bb_http_route_handler_cap());

    for (int i = 0; i < s_extender_count; i++) {
        s_extenders[i](root);
    }

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

static const bb_route_response_t s_info_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"board\":{\"type\":\"string\"},"
      "\"project_name\":{\"type\":\"string\"},"
      "\"version\":{\"type\":\"string\"},"
      "\"idf_version\":{\"type\":\"string\"},"
      "\"build_date\":{\"type\":\"string\"},"
      "\"build_time\":{\"type\":\"string\"},"
      "\"chip_model\":{\"type\":\"string\"},"
      "\"cores\":{\"type\":\"integer\"},"
      "\"mac\":{\"type\":\"string\"},"
      "\"flash_size\":{\"type\":\"integer\"},"
      "\"total_heap\":{\"type\":\"integer\"},"
      "\"free_heap\":{\"type\":\"integer\"},"
      "\"app_size\":{\"type\":\"integer\"},"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"ota_validated\":{\"type\":\"boolean\"},"
      "\"heap_free_total\":{\"type\":\"integer\"},"
      "\"heap_free_internal\":{\"type\":\"integer\"},"
      "\"heap_minimum_ever\":{\"type\":\"integer\"},"
      "\"chip_revision\":{\"type\":\"integer\"},"
      "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
      "\"network\":{\"type\":\"object\","
      "\"properties\":{"
      "\"ssid\":{\"type\":\"string\"},"
      "\"bssid\":{\"type\":\"string\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"ip\":{\"type\":\"string\"},"
      "\"connected\":{\"type\":\"boolean\"},"
      "\"disc_reason\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"retry_count\":{\"type\":\"integer\"}}}},"
      "\"required\":[\"board\",\"version\",\"network\"]}",
      "full device info including board and network" },
    { 0 },
};

static const bb_route_t s_info_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/info",
    .tag      = "info",
    .summary  = "Get full device info",
    .responses = s_info_responses,
    .handler  = info_handler,
};

static bb_err_t bb_info_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    s_frozen = true;
    bb_err_t err = bb_http_register_described_route(server, &s_info_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "info route registered");
    return BB_OK;
}

#if CONFIG_BB_INFO_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_info, bb_info_init, 1);
#endif
