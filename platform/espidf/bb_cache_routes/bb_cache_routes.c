#include "bb_cache_routes.h"
#include "bb_cache.h"
#include "cache_route_status.h"

#include <string.h>

#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_log.h"

static const char *TAG = "bb_cache_routes";

// ---------------------------------------------------------------------------
// Kconfig bridge (pattern from bb_cache.h) — CONFIG_BB_CACHE_ROUTES_BUF_BYTES
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_ROUTES_BUF_BYTES
#define BB_CACHE_ROUTES_BUF_BYTES CONFIG_BB_CACHE_ROUTES_BUF_BYTES
#endif
#endif
#ifndef BB_CACHE_ROUTES_BUF_BYTES
#define BB_CACHE_ROUTES_BUF_BYTES 768
#endif

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

static bb_err_t cache_handler(bb_http_request_t *req)
{
    // Parse required ?key= query parameter (?key=<key> — query-param, not
    // path-param, because bb_dispatch_api is exact-match only; mirrors the
    // ?topic= idiom on GET /api/events).
    char key[BB_CACHE_KEY_MAX] = {0};
    if (bb_http_req_query_key_value(req, "key", key, sizeof(key)) != BB_OK) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing or invalid key");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    char   buf[BB_CACHE_ROUTES_BUF_BYTES];
    size_t len = 0;
    bb_err_t rc = bb_cache_get_serialized(key, buf, sizeof(buf), &len);
    int status = cache_route_map_status(rc);

    if (status != 200) {
        if (status == 500) {
            bb_log_e(TAG, "get_serialized('%s') failed: rc=%d", key, (int)rc);
        }
        bb_http_resp_set_status(req, status);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error",
                                      (status == 404) ? "not found" : "internal error");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, buf, (int)len);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    return err;
}

// ---------------------------------------------------------------------------
// Route descriptor + registration
// ---------------------------------------------------------------------------

static const bb_route_response_t s_cache_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"ts_ms\":{\"type\":\"integer\"},\"data\":{\"type\":\"object\"}},"
      "\"required\":[\"ts_ms\",\"data\"]}",
      "The key's enveloped {ts_ms,data} snapshot, verbatim from bb_cache." },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing ?key= query parameter" },
    { 404, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "key not registered, or registered but no snapshot yet" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "serialize/buffer failure" },
    { 0 },
};

static const bb_route_param_t s_cache_params[] = {
    {
        .name        = "key",
        .in          = "query",
        .description = "bb_cache key to fetch. Available keys are the registered "
                       "bb_cache_register() keys for this firmware.",
        .required    = true,
        .schema_type = "string",
    },
};

static const bb_route_t s_cache_route = {
    .method            = BB_HTTP_GET,
    .path              = "/api/cache",
    .tag               = "cache",
    .summary           = "Fetch a bb_cache key's enveloped snapshot",
    .responses         = s_cache_responses,
    .handler           = cache_handler,
    .parameters        = s_cache_params,
    .parameters_count  = 1,
};

bb_err_t bb_cache_routes_init(bb_http_handle_t server)
{
    bb_http_reserve_routes(1);  // GET /api/cache
    return bb_http_register_described_route(server, &s_cache_route);
}
