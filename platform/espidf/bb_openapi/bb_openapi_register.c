#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_system.h"

#include "esp_http_server.h"

#include <string.h>

static const char *TAG = "bb_openapi";

// user_ctx holds a pointer to the caller-supplied bb_openapi_meta_t
static esp_err_t openapi_handler(httpd_req_t *req)
{
    const bb_openapi_meta_t *meta = (const bb_openapi_meta_t *)req->user_ctx;

    bb_openapi_meta_t effective = *meta;
    if (!effective.version) effective.version = bb_system_get_version();

    bb_json_t doc = bb_openapi_emit(&effective);
    if (!doc) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "openapi emit failed");
        return ESP_FAIL;
    }

    char *json = bb_json_serialize(doc);
    bb_json_free(doc);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "openapi serialize failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    bb_json_free_str(json);

    return err;
}

// ---------------------------------------------------------------------------
// Route descriptor (self-describing; handler registered via raw httpd API)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_openapi_responses[] = {
    { 200, "application/json", NULL, "OpenAPI 3.1 spec for all described routes" },
    { 0 },
};

static const bb_route_t s_openapi_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/openapi.json",
    .tag      = "system",
    .summary  = "Get OpenAPI spec",
    .responses = s_openapi_responses,
    .handler  = NULL,
};

bb_err_t bb_openapi_register(bb_http_handle_t server, const bb_openapi_meta_t *meta)
{
    if (!server || !meta) return BB_ERR_INVALID_ARG;

    httpd_handle_t h = (httpd_handle_t)server;

    httpd_uri_t uri = {
        .uri      = "/api/openapi.json",
        .method   = HTTP_GET,
        .handler  = openapi_handler,
        .user_ctx = (void *)meta,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to register /api/openapi.json: %d", err);
        return (bb_err_t)err;
    }

    // Add descriptor for OpenAPI spec emission (self-describing).
    bb_http_register_route_descriptor_only(&s_openapi_route);

    bb_log_i(TAG, "registered GET /api/openapi.json");
    return BB_OK;
}
