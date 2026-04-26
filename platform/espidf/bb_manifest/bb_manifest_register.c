#include "bb_manifest.h"
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

    bb_log_i(TAG, "registered GET /api/manifest");
    return BB_OK;
}
