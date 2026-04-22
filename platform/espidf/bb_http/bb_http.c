#include "bb_http.h"
#include "esp_http_server.h"
#include "bb_log.h"
#include "esp_wifi.h"
#include "bb_nv.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

static const char *s_cors_methods = "GET, POST, OPTIONS";
static const char *s_cors_headers = "Content-Type";

void bb_http_set_cors_methods(const char *methods)
{
    s_cors_methods = methods ? methods : "GET, POST, OPTIONS";
}

void bb_http_set_cors_headers(const char *headers)
{
    s_cors_headers = headers ? headers : "Content-Type";
}

static esp_err_t preflight_handler(httpd_req_t *req);

// Ensure the shared HTTP server is started. Exposed for bb_prov to call.
// Low-level helper; most consumers should use bb_http_server_start instead.
bb_err_t bb_http_server_ensure_started(void)
{
    if (s_server) return BB_OK;
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
    return BB_OK;
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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", s_cors_methods);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", s_cors_headers);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}



esp_err_t bb_http_server_start(void)
{
    esp_err_t err = bb_http_server_ensure_started();
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
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

// Static asset handler: reads the asset from user_ctx and emits it with headers
static esp_err_t asset_handler(httpd_req_t *req)
{
    const bb_http_asset_t *asset = (const bb_http_asset_t*)req->user_ctx;
    if (!asset) return ESP_FAIL;

    // Set Content-Type
    httpd_resp_set_type(req, asset->mime);

    // Set Content-Encoding if present
    if (asset->encoding) {
        httpd_resp_set_hdr(req, "Content-Encoding", asset->encoding);
    }

    // Set Cache-Control
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");

    // Send body
    esp_err_t err = httpd_resp_send(req, (const char*)asset->data, asset->len);
    return err;
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
        case BB_HTTP_PATCH:
            http_method_mapped = HTTP_PATCH;
            break;
        case BB_HTTP_PUT:
            http_method_mapped = HTTP_PUT;
            break;
        case BB_HTTP_DELETE:
            http_method_mapped = HTTP_DELETE;
            break;
        case BB_HTTP_OPTIONS:
            http_method_mapped = HTTP_OPTIONS;
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

bb_err_t bb_http_resp_send_err(bb_http_request_t *req, int status_code, const char *message)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req) return BB_ERR_INVALID_ARG;

    // Map status code to httpd_err_code_t
    httpd_err_code_t err_code;
    switch (status_code) {
        case 400:
            err_code = HTTPD_400_BAD_REQUEST;
            break;
        case 404:
            err_code = HTTPD_404_NOT_FOUND;
            break;
        case 405:
            err_code = HTTPD_405_METHOD_NOT_ALLOWED;
            break;
        case 408:
            err_code = HTTPD_408_REQ_TIMEOUT;
            break;
        case 411:
            err_code = HTTPD_411_LENGTH_REQUIRED;
            break;
        case 414:
            err_code = HTTPD_414_URI_TOO_LONG;
            break;
        case 431:
            err_code = HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE;
            break;
        case 500:
            err_code = HTTPD_500_INTERNAL_SERVER_ERROR;
            break;
        case 501:
            err_code = HTTPD_501_METHOD_NOT_IMPLEMENTED;
            break;
        case 505:
            err_code = HTTPD_505_VERSION_NOT_SUPPORTED;
            break;
        default:
            err_code = HTTPD_500_INTERNAL_SERVER_ERROR;
            break;
    }

    esp_err_t err = httpd_resp_send_err(http_req, err_code, message);
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

bb_err_t bb_http_register_assets(bb_http_handle_t server,
                                 const bb_http_asset_t *assets,
                                 size_t n)
{
    httpd_handle_t h = (httpd_handle_t)server;
    if (!h || !assets) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < n; i++) {
        const bb_http_asset_t *asset = &assets[i];
        if (!asset->path || !asset->mime || !asset->data) {
            return BB_ERR_INVALID_ARG;
        }

        httpd_uri_t uri = {
            .uri = asset->path,
            .method = HTTP_GET,
            .handler = asset_handler,
            .user_ctx = (void*)asset,
        };

        esp_err_t err = httpd_register_uri_handler(h, &uri);
        if (err != ESP_OK) {
            bb_log_e(TAG, "failed to register asset %s: %s", asset->path, esp_err_to_name(err));
            return BB_ERR_INVALID_ARG;
        }
    }

    return BB_OK;
}

// No-op on ESP-IDF; service loop runs in httpd task
void bb_http_server_poll(void)
{
    // ESP-IDF httpd runs on its own FreeRTOS task; nothing to do here
}
