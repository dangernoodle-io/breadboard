#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_system.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_openapi";

// Module-level meta pointer set at registration time.
static const bb_openapi_meta_t *s_meta = NULL;

static bb_err_t openapi_handler(bb_http_request_t *req)
{
    // Build effective metadata with fallbacks for missing fields.
    bb_openapi_meta_t effective = {};
    if (s_meta) {
        effective = *s_meta;
    }
    if (!effective.title) effective.title = "breadboard device";
    if (!effective.version) effective.version = bb_system_get_version();

    bb_json_t doc = bb_openapi_emit(&effective);
    if (!doc) {
        bb_http_resp_send_err(req, 500, "openapi emit failed");
        return BB_ERR_INVALID_STATE;
    }

    bb_err_t err = bb_http_resp_send_json(req, doc);
    bb_json_free(doc);
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

void bb_openapi_set_meta(const bb_openapi_meta_t *meta)
{
    s_meta = meta;
}

static bb_err_t bb_openapi_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

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

#if CONFIG_BB_OPENAPI_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_openapi, bb_openapi_init, 1);
#endif
