#include "bb_manifest.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_manifest";

static bb_err_t manifest_handler(bb_http_request_t *req)
{
    bb_json_t doc = bb_manifest_emit();
    if (!doc) {
        bb_http_resp_send_err(req, 500, "manifest emit failed");
        return BB_ERR_INVALID_STATE;
    }

    char *json = bb_json_serialize(doc);
    bb_json_free(doc);

    if (!json) {
        bb_http_resp_send_err(req, 500, "manifest serialize failed");
        return BB_ERR_INVALID_STATE;
    }

    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_sendstr(req, json);
    bb_json_free_str(json);

    return err;
}

// ---------------------------------------------------------------------------
// Route descriptor (self-describing; handler registered via bb_http_register_route)
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

static bb_err_t bb_manifest_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_route(server, BB_HTTP_GET,
                                          "/api/manifest", manifest_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/manifest: %d", err);
        return err;
    }

    // Add descriptor for OpenAPI spec emission (self-describing).
    bb_http_register_route_descriptor_only(&s_manifest_route);

    bb_log_i(TAG, "registered GET /api/manifest");
    return BB_OK;
}

#if CONFIG_BB_MANIFEST_AUTOREGISTER
BB_REGISTRY_REGISTER(bb_manifest, bb_manifest_init);
#endif
