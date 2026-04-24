#include "bb_http.h"
#include "bb_wifi.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "cJSON.h"
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

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(ap, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, ap);
    }
    char *json = cJSON_PrintUnformatted(arr);
    bb_http_resp_set_header(req, "Content-Type", "application/json");
    bb_err_t rc = bb_http_resp_send(req, json ? json : "[]", json ? strlen(json) : 2);
    if (json) cJSON_free(json);
    cJSON_Delete(arr);
    return rc;
}
#endif

bb_err_t bb_http_register_common_routes(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc;
    rc = bb_http_register_route(server, BB_HTTP_GET, "/api/version", version_handler);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_route(server, BB_HTTP_GET, "/api/ping", ping_handler);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_route(server, BB_HTTP_POST, "/api/reboot", reboot_handler);
    if (rc != BB_OK) return rc;
#ifdef ESP_PLATFORM
    rc = bb_http_register_route(server, BB_HTTP_GET, "/api/scan", scan_handler);
    if (rc != BB_OK) return rc;
#endif
    return BB_OK;
}
