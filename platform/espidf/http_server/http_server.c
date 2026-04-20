#include "http_server.h"
#include "esp_http_server.h"
#include "log_stream.h"
#include "esp_wifi.h"
#include "nv_config.h"
#include "wifi_prov.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;
static bb_http_app_routes_fn s_app_routes_fn = NULL;


static esp_err_t preflight_handler(httpd_req_t *req);

static esp_err_t ensure_server_started(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 32;
    config.stack_size = 6144;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;

    httpd_uri_t preflight = { .uri = "/*", .method = HTTP_OPTIONS, .handler = preflight_handler };
    httpd_register_uri_handler(s_server, &preflight);
    return ESP_OK;
}

// Internal helper: cast opaque handle back to httpd_handle_t for internal use
static inline httpd_handle_t _bb_handle_to_internal(bb_http_handle_t h) {
    return (httpd_handle_t)h;
}

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

static esp_err_t preflight_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
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

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);

    // Signal provisioning complete
    bb_wifi_prov_signal_done();

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


esp_err_t bb_http_server_start_prov(bb_http_app_routes_fn prov_ui_routes_fn)
{
    esp_err_t err = ensure_server_started();
    if (err != ESP_OK) return err;

    s_app_routes_fn = prov_ui_routes_fn;

    httpd_uri_t prov_save = { .uri = "/save", .method = HTTP_POST, .handler = prov_save_handler };
    httpd_uri_t prov_redirect = { .uri = "/*", .method = HTTP_GET, .handler = prov_redirect_handler };

    httpd_register_uri_handler(s_server, &prov_save);

    // Register consumer routes (GET /, favicon, etc) before the GET /*
    // captive-portal wildcard so specific handlers take precedence.
    if (prov_ui_routes_fn) {
        prov_ui_routes_fn((bb_http_handle_t)s_server);
    }

    httpd_register_uri_handler(s_server, &prov_redirect);

    bb_log_i(TAG, "provisioning server started on port 80");
    return ESP_OK;
}

void bb_http_server_switch_to_normal(bb_http_app_routes_fn app_routes_fn)
{
    if (!s_server) return;

    // Unregister only breadboard's prov handlers: /save (POST) and /* (GET catch-all)
    httpd_unregister_uri_handler(s_server, "/save", HTTP_POST);
    httpd_unregister_uri_handler(s_server, "/*", HTTP_GET);

    s_app_routes_fn = app_routes_fn;

    // Register app routes if callback provided
    if (app_routes_fn) {
        app_routes_fn((bb_http_handle_t)s_server);
    }
}

esp_err_t bb_http_server_start(bb_http_app_routes_fn app_routes_fn)
{
    esp_err_t err = ensure_server_started();
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    s_app_routes_fn = app_routes_fn;

    // Register app routes if callback provided
    if (app_routes_fn) {
        app_routes_fn((bb_http_handle_t)s_server);
    }

    bb_log_i(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t bb_http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}

bb_http_handle_t bb_http_server_get_handle(void)
{
    return (bb_http_handle_t)s_server;
}
