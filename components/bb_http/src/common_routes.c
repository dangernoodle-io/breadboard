#include "bb_http.h"
#include "bb_wifi.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "bb_json.h"
#include "esp_timer.h"
#endif

static bb_err_t version_handler(bb_http_request_t *req)
{
    char version[32];
    version[0] = '\0';
    bb_system_get_version(version, sizeof(version));
    bb_http_resp_set_header(req, "Content-Type", "text/plain");
    return bb_http_resp_send(req, version, strlen(version));
}

static bb_err_t reboot_handler(bb_http_request_t *req)
{
    static const char body[] = "{\"status\":\"rebooting\"}";
    bb_http_resp_set_header(req, "Content-Type", "application/json");
    bb_err_t rc = bb_http_resp_send(req, body, sizeof(body) - 1);
    bb_system_reboot();
    return rc;
}

static bb_err_t ping_handler(bb_http_request_t *req)
{
    char body[32];
#ifdef ESP_PLATFORM
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
#else
    uint32_t uptime_s = 0;  /* host/arduino: stub — no uptime source */
#endif
    int n = snprintf(body, sizeof(body), "ok %" PRIu32, uptime_s);
    if (n < 0 || (size_t)n >= sizeof(body)) n = 2;  /* safe fallback to "ok" */
    bb_http_resp_set_header(req, "Content-Type", "text/plain");
    return bb_http_resp_send(req, body, (size_t)n);
}

#ifdef ESP_PLATFORM
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
    char *json = bb_json_serialize(arr);
    bb_http_resp_set_header(req, "Content-Type", "application/json");
    bb_err_t rc = bb_http_resp_send(req, json ? json : "[]", json ? strlen(json) : 2);
    if (json) bb_json_free_str(json);
    bb_json_free(arr);
    return rc;
}
#endif

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_version_responses[] = {
    { 200, "text/plain", NULL, "firmware version string" },
    { 0 },
};

static const bb_route_t s_version_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/version",
    .tag      = "system",
    .summary  = "Get firmware version",
    .responses = s_version_responses,
    .handler  = version_handler,
};

static const bb_route_response_t s_ping_responses[] = {
    { 200, "text/plain", NULL, "ok <uptime_seconds>" },
    { 0 },
};

static const bb_route_t s_ping_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/ping",
    .tag      = "system",
    .summary  = "Liveness check",
    .responses = s_ping_responses,
    .handler  = ping_handler,
};

static const bb_route_response_t s_reboot_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "reboot acknowledged" },
    { 0 },
};

static const bb_route_t s_reboot_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/reboot",
    .tag      = "system",
    .summary  = "Reboot the device",
    .responses = s_reboot_responses,
    .handler  = reboot_handler,
};

#ifdef ESP_PLATFORM
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
#endif

bb_err_t bb_http_register_common_routes(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc;
    rc = bb_http_register_described_route(server, &s_version_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_ping_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_reboot_route);
    if (rc != BB_OK) return rc;
#ifdef ESP_PLATFORM
    rc = bb_http_register_described_route(server, &s_scan_route);
    if (rc != BB_OK) return rc;
#endif
    return BB_OK;
}
