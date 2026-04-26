#include "bb_manifest.h"
#include "bb_http.h"
#include "bb_log.h"

#include "esp_http_server.h"

#include <string.h>

static const char *TAG = "bb_manifest";

// user_ctx is unused but kept for consistency with other route registrations
static esp_err_t manifest_handler(httpd_req_t *req)
{
    bb_json_t doc = bb_manifest_emit();
    if (!doc) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "manifest emit failed");
        return ESP_FAIL;
    }

    char *json = bb_json_serialize(doc);
    bb_json_free(doc);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "manifest serialize failed");
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

static const bb_route_response_t s_manifest_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"nvs\":{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"namespace\":{\"type\":\"string\"},"
      "\"keys\":{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"key\":{\"type\":\"string\"},"
      "\"type\":{\"type\":\"string\"},"
      "\"default\":{\"type\":\"string\"},"
      "\"desc\":{\"type\":\"string\"},"
      "\"reboot_required\":{\"type\":\"boolean\"},"
      "\"provisioning_only\":{\"type\":\"boolean\"}}"
      "}}}},"
      "\"mdns\":{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"service\":{\"type\":\"string\"},"
      "\"txt\":{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"key\":{\"type\":\"string\"},"
      "\"desc\":{\"type\":\"string\"},"
      "\"values\":{\"type\":\"string\"}}}}}}}}",
      "NVS key manifest and mDNS TXT key manifest" },
    { 0 },
};

static const bb_route_t s_manifest_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/manifest",
    .tag      = "manifest",
    .summary  = "Get NVS and mDNS key manifest",
    .responses = s_manifest_responses,
    .handler  = NULL,
};

bb_err_t bb_manifest_register_route(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    httpd_handle_t h = (httpd_handle_t)server;

    httpd_uri_t uri = {
        .uri      = "/api/manifest",
        .method   = HTTP_GET,
        .handler  = manifest_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to register /api/manifest: %d", err);
        return (bb_err_t)err;
    }

    // Add descriptor for OpenAPI spec emission (self-describing).
    bb_http_register_route_descriptor_only(&s_manifest_route);

    bb_log_i(TAG, "registered GET /api/manifest");
    return BB_OK;
}
