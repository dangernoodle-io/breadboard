#include "bb_prov.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "bb_prov";

// AP and provisioning state
static esp_netif_t *s_ap_netif = NULL;
static volatile bool s_dns_running = false;
static TaskHandle_t s_dns_task_handle = NULL;
static char s_ap_ssid[32];
static EventGroupHandle_t s_prov_event_group = NULL;

// AP SSID prefix (default "BB-")
static char s_ap_ssid_prefix[16] = "BB-";
static bool s_ap_ssid_prefix_set = false;
// AP password (default "breadboard")
static char s_ap_password[64] = "breadboard";
static bb_prov_save_cb_t s_save_cb = NULL;

void bb_prov_set_save_callback(bb_prov_save_cb_t cb) { s_save_cb = cb; }

#define PROV_DONE_BIT BIT0

static void set_common_headers(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Connection", "close");
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");
}

// Handle provisioning form submission
static bb_err_t prov_save_handler(bb_http_request_t *req)
{
    set_common_headers(req);
    char body[512];

    // Validate content length to prevent silent body truncation
    int content_len = bb_http_req_body_len(req);
    if (content_len > (int)(sizeof(body) - 1)) {
        bb_http_resp_send_err(req, 400, "Body too large");
        return BB_ERR_INVALID_ARG;
    }
    int len = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        bb_http_resp_send_err(req, 400, "Empty body");
        return BB_ERR_INVALID_ARG;
    }
    body[len] = '\0';

    // Parse URL-encoded fields
    char ssid[32] = "", pass[64] = "";
    switch (bb_prov_parse_body(body, len, ssid, sizeof(ssid), pass, sizeof(pass))) {
        case BB_PROV_PARSE_EMPTY_BODY:
            bb_http_resp_send_err(req, 400, "Empty body");
            return BB_ERR_INVALID_ARG;
        case BB_PROV_PARSE_SSID_REQUIRED:
            bb_http_resp_send_err(req, 400, "SSID required");
            return BB_ERR_INVALID_ARG;
        case BB_PROV_PARSE_OK:
            break;
    }

    bb_err_t err = bb_nv_config_set_wifi(ssid, pass);
    if (err != BB_OK) {
        bb_http_resp_send_err(req, 500, "Failed to save config");
        return BB_ERR_INVALID_STATE;
    }

    if (s_save_cb) {
        bb_err_t cb_err = s_save_cb(req, body, len);
        if (cb_err != BB_OK) return BB_ERR_INVALID_STATE;
    } else {
        bb_http_resp_set_status(req, 204);
        bb_http_resp_send(req, NULL, 0);
    }

    bb_prov_signal_done();
    return BB_OK;
}

static bb_err_t prov_redirect_handler(bb_http_request_t *req)
{
    set_common_headers(req);
    bb_http_resp_set_status(req, 302);
    bb_http_resp_set_header(req, "Location", "http://192.168.4.1/");
    bb_http_resp_send(req, NULL, 0);
    return BB_OK;
}

// Captive DNS task - responds to all DNS queries with 192.168.4.1
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        bb_log_e(TAG, "failed to create DNS socket");
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        bb_log_e(TAG, "failed to bind DNS socket");
        close(sock);
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Set receive timeout so we can check s_dns_running
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bb_log_i(TAG, "captive DNS listening on 0.0.0.0:53");

    uint8_t buf[256];
    while (s_dns_running) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &client_len);

        if (len < 12) {
            // Too short for DNS header or timeout
            continue;
        }

        // Build response: copy query, set response flags, add A record pointing to 192.168.4.1
        uint8_t resp[256];
        memcpy(resp, buf, len);
        resp[2] = 0x81;  // QR=1 (response)
        resp[3] = 0x80;  // AA=1 (authoritative), no error

        resp[6] = 0;     // ANCOUNT high byte
        resp[7] = 1;     // ANCOUNT = 1 answer

        // Append answer: name pointer + type A + class IN + TTL + rdlength + IP
        int pos = len;
        resp[pos++] = 0xC0;  // pointer to question name
        resp[pos++] = 0x0C;
        resp[pos++] = 0x00;  // type A
        resp[pos++] = 0x01;
        resp[pos++] = 0x00;  // class IN
        resp[pos++] = 0x01;
        resp[pos++] = 0x00;  // TTL 60 seconds
        resp[pos++] = 0x00;
        resp[pos++] = 0x00;
        resp[pos++] = 0x3C;
        resp[pos++] = 0x00;  // rdlength 4
        resp[pos++] = 0x04;
        resp[pos++] = 192;   // 192.168.4.1
        resp[pos++] = 168;
        resp[pos++] = 4;
        resp[pos++] = 1;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&client, client_len);
    }

    close(sock);
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

bb_err_t bb_prov_start_ap(void)
{
    // Create provisioning event group if not already created
    if (s_prov_event_group == NULL) {
        s_prov_event_group = xEventGroupCreate();
    }

    ESP_ERROR_CHECK(bb_wifi_ensure_netif());

    // Create AP netif with default config (auto-starts DHCPS)
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        bb_log_e(TAG, "failed to create AP netif");
        return ESP_FAIL;
    }

    // Initialize WiFi and set mode before reading AP MAC
    // (hosted/wifi_remote populates per-interface MACs after mode is set; reading
    // ESP_MAC_WIFI_SOFTAP before set_mode returns ESP_ERR_NOT_SUPPORTED on P4+C6)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Build AP SSID from MAC address
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    const char *prefix = s_ap_ssid_prefix_set ? s_ap_ssid_prefix : "BB-";
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X", prefix, mac[4], mac[5]);
    strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';

    // Configure AP
    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_config.ap.password, s_ap_password, sizeof(ap_config.ap.password));
    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start DNS task
    s_dns_running = true;
    BaseType_t xReturned = xTaskCreatePinnedToCore(
        dns_task,
        "dns",
        4096,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_dns_task_handle,
        0
    );

    if (xReturned != pdPASS) {
        bb_log_e(TAG, "failed to create DNS task");
        s_dns_running = false;
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        return ESP_FAIL;
    }

    bb_log_i(TAG, "AP started: SSID=%s, password=%s", ssid, s_ap_password);

    return ESP_OK;
}

void bb_prov_stop_ap(void)
{
    if (s_ap_netif == NULL) {
        bb_log_w(TAG, "AP not initialized");
        return;
    }

    // Stop DNS task
    s_dns_running = false;
    if (s_dns_task_handle != NULL) {
        // Wait for task to exit with a timeout
        for (int i = 0; i < 50; i++) {  // 5 second timeout (50 * 100ms)
            if (s_dns_task_handle == NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // Stop WiFi
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    // Destroy AP netif
    esp_netif_destroy(s_ap_netif);
    s_ap_netif = NULL;

    bb_log_i(TAG, "AP stopped");
}

void bb_prov_get_ap_ssid(char *buf, size_t len)
{
    strncpy(buf, s_ap_ssid, len - 1);
    buf[len - 1] = '\0';
}

bool bb_prov_wait_done(uint32_t timeout_ms)
{
    if (s_prov_event_group == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_prov_event_group, PROV_DONE_BIT, pdTRUE, pdTRUE, ticks);
    return (bits & PROV_DONE_BIT) != 0;
}

void bb_prov_signal_done(void)
{
    if (s_prov_event_group != NULL) {
        xEventGroupSetBits(s_prov_event_group, PROV_DONE_BIT);
    }
}

bb_err_t bb_prov_start(const bb_http_asset_t *assets, size_t n,
                       bb_prov_extra_routes_fn_t extra)
{
    // Ensure the shared HTTP server is started (internal helper)
    bb_err_t err = bb_http_server_ensure_started();
    if (err != BB_OK) return err;

    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return BB_ERR_INVALID_STATE;

    bb_http_register_route(server, BB_HTTP_POST, "/save", prov_save_handler);

    // Register consumer assets (caller MUST supply at least one asset with path="/")
    if (assets && n > 0) {
        bb_http_register_assets(server, assets, n);
    }

    // Register built-in common routes so the prov UI gets /api/version,
    // /api/scan, /api/reboot without the consumer wiring anything.
    bb_err_t rc = bb_http_register_common_routes(server);
    if (rc != BB_OK) return rc;

    // Consumer's dynamic endpoints (e.g. advanced-UI backing routes).
    if (extra) {
        rc = extra(server);
        if (rc != BB_OK) return rc;
    }

    // Captive-portal wildcard LAST so all specific GETs win first-match.
    bb_http_register_route(server, BB_HTTP_GET, "/*", prov_redirect_handler);

    bb_log_i(TAG, "provisioning server started on port 80");

    // Prefetch the SSID list so the portal's auto-scan on first page-load
    // returns a populated array instead of an empty cache.
    bb_wifi_scan_start_async();

    return BB_OK;
}

void bb_prov_stop(void)
{
    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return;

    // Unregister provisioning handlers: /save (POST) and /* (GET catch-all)
    bb_http_unregister_route(server, BB_HTTP_POST, "/save");
    bb_http_unregister_route(server, BB_HTTP_GET, "/*");
}

void bb_prov_set_ap_ssid_prefix(const char *prefix)
{
    if (!prefix) {
        s_ap_ssid_prefix[0] = '\0';
        s_ap_ssid_prefix_set = false;
        return;
    }
    strncpy(s_ap_ssid_prefix, prefix, sizeof(s_ap_ssid_prefix) - 1);
    s_ap_ssid_prefix[sizeof(s_ap_ssid_prefix) - 1] = '\0';
    s_ap_ssid_prefix_set = true;
}

void bb_prov_set_ap_password(const char *password)
{
    if (!password) password = "breadboard";
    strncpy(s_ap_password, password, sizeof(s_ap_password) - 1);
    s_ap_password[sizeof(s_ap_password) - 1] = '\0';
}
