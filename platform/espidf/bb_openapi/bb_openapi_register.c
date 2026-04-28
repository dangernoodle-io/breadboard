#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_system.h"

#include <string.h>

static const char *TAG = "bb_openapi";

// Module-level meta pointer set at registration time.
static const bb_openapi_meta_t *s_meta = NULL;

static bb_err_t openapi_handler(bb_http_request_t *req)
{
    bb_openapi_meta_t effective = *s_meta;
    if (!effective.version) effective.version = bb_system_get_version();

    bb_json_t doc = bb_openapi_emit(&effective);
    if (!doc) {
        bb_http_resp_send_err(req, 500, "openapi emit failed");
        return BB_ERR_INVALID_STATE;
    }

    char *json = bb_json_serialize(doc);
    bb_json_free(doc);

    if (!json) {
        bb_http_resp_send_err(req, 500, "openapi serialize failed");
        return BB_ERR_INVALID_STATE;
    }

    bb_http_resp_set_header(req, "Content-Type", "application/json");
    bb_err_t err = bb_http_resp_sendstr(req, json);
    bb_json_free_str(json);

    return err;
}

// ---------------------------------------------------------------------------
// Route descriptor (self-describing; handler registered via bb_http_register_route)
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

    s_meta = meta;

    bb_err_t err = bb_http_register_route(server, BB_HTTP_GET,
                                          "/api/openapi.json", openapi_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/openapi.json: %d", err);
        return err;
    }

    // Add descriptor for OpenAPI spec emission (self-describing).
    bb_http_register_route_descriptor_only(&s_openapi_route);

    bb_log_i(TAG, "registered GET /api/openapi.json");
    return BB_OK;
}
