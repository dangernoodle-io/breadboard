#include "bb_prov.h"
#include "bb_http.h"
#include "esp_http_server.h"
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

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

// Handle provisioning form submission
static esp_err_t prov_save_handler(httpd_req_t *req)
{
    set_common_headers(req);
    char body[512];

    // Validate content length to prevent silent body truncation
    if (req->content_len > sizeof(body) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    // Parse URL-encoded fields
    char ssid[32] = "", pass[64] = "";
    switch (bb_prov_parse_body(body, len, ssid, sizeof(ssid), pass, sizeof(pass))) {
        case BB_PROV_PARSE_EMPTY_BODY:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
            return ESP_FAIL;
        case BB_PROV_PARSE_SSID_REQUIRED:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
            return ESP_FAIL;
        case BB_PROV_PARSE_OK:
            break;
    }

    esp_err_t err = bb_nv_config_set_wifi(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
        return ESP_FAIL;
    }

    if (s_save_cb) {
        bb_err_t cb_err = s_save_cb((bb_http_request_t *)req, body, len);
        if (cb_err != BB_OK) return ESP_FAIL;
    } else {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
    }

    bb_prov_signal_done();
    return ESP_OK;
}

static esp_err_t prov_redirect_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
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

esp_err_t bb_prov_start_ap(void)
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

esp_err_t bb_prov_start(const bb_http_asset_t *assets, size_t n)
{
    // Ensure the shared HTTP server is started (internal helper)
    esp_err_t err = bb_http_server_ensure_started();
    if (err != ESP_OK) return err;

    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return ESP_FAIL;

    httpd_uri_t prov_save = { .uri = "/save", .method = HTTP_POST, .handler = prov_save_handler };
    httpd_uri_t prov_redirect = { .uri = "/*", .method = HTTP_GET, .handler = prov_redirect_handler };

    httpd_register_uri_handler((httpd_handle_t)server, &prov_save);

    // Register consumer assets (caller MUST supply at least one asset with path="/")
    if (assets && n > 0) {
        bb_http_register_assets(server, assets, n);
    }

    // Register captive-portal redirect LAST so specific asset paths win
    httpd_register_uri_handler((httpd_handle_t)server, &prov_redirect);

    bb_log_i(TAG, "provisioning server started on port 80");
    return ESP_OK;
}

void bb_prov_stop(void)
{
    bb_http_handle_t server = bb_http_server_get_handle();
    if (!server) return;

    httpd_handle_t h = (httpd_handle_t)server;

    // Unregister provisioning handlers: /save (POST) and /* (GET catch-all)
    httpd_unregister_uri_handler(h, "/save", HTTP_POST);
    httpd_unregister_uri_handler(h, "/*", HTTP_GET);
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
