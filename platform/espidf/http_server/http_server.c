#include "http_server.h"
#include "esp_http_server.h"
#include "log_stream.h"
#include "esp_wifi.h"
#include "nv_config.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;
static bb_http_app_routes_fn s_app_routes_fn = NULL;


static esp_err_t preflight_handler(httpd_req_t *req);

// Ensure the shared HTTP server is started. Exposed for bb_prov to call.
// Low-level helper; most consumers should use bb_http_server_start instead.
esp_err_t bb_http_server_ensure_started(void)
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



esp_err_t bb_http_server_start(bb_http_app_routes_fn app_routes_fn)
{
    esp_err_t err = bb_http_server_ensure_started();
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

// ============================================================================
// PORTABLE API IMPLEMENTATIONS
// ============================================================================

// Shim handler adapter: translates bb_http_handler_fn ↔ httpd_req_t*
static esp_err_t bb_shim_handler(httpd_req_t *req)
{
    bb_http_handler_fn fn = (bb_http_handler_fn)req->user_ctx;
    if (!fn) return ESP_FAIL;
    return fn((bb_http_request_t*)req) == BB_OK ? ESP_OK : ESP_FAIL;
}

bb_err_t bb_http_register_route(bb_http_handle_t server,
                                bb_http_method_t method,
                                const char *path,
                                bb_http_handler_fn handler)
{
    httpd_handle_t h = (httpd_handle_t)server;
    if (!h || !handler) return BB_ERR_INVALID_ARG;

    // Map method (HTTP_GET, HTTP_POST are enum constants)
    int http_method_mapped;
    switch (method) {
        case BB_HTTP_GET:
            http_method_mapped = HTTP_GET;
            break;
        case BB_HTTP_POST:
            http_method_mapped = HTTP_POST;
            break;
        default:
            return BB_ERR_INVALID_ARG;
    }

    // Build httpd_uri_t with handler stored in user_ctx
    httpd_uri_t uri = {
        .uri = path,
        .method = http_method_mapped,
        .handler = bb_shim_handler,
        .user_ctx = (void*)handler,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req) return BB_ERR_INVALID_ARG;

    // Convert status code to string
    char status_str[32];
    snprintf(status_str, sizeof(status_str), "%d", status_code);
    esp_err_t err = httpd_resp_set_status(http_req, status_str);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req || !key || !value) return BB_ERR_INVALID_ARG;

    esp_err_t err = httpd_resp_set_hdr(http_req, key, value);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_resp_send(bb_http_request_t *req, const char *body, size_t len)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req) return BB_ERR_INVALID_ARG;

    esp_err_t err = httpd_resp_send(http_req, body, len);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

int bb_http_req_body_len(bb_http_request_t *req)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req) return -1;
    return http_req->content_len;
}

int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req || !buf) return -1;

    return httpd_req_recv(http_req, buf, buf_size);
}

// No-op on ESP-IDF; service loop runs in httpd task
void bb_http_server_poll(void)
{
    // ESP-IDF httpd runs on its own FreeRTOS task; nothing to do here
}
