#include "bb_log.h"
#include "bb_http.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_log_http";

static bb_err_t log_level_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid request");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "read failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    char tag[64];
    char level_str[16];
    bb_url_decode_field(body, "tag", tag, sizeof(tag));
    bb_url_decode_field(body, "level", level_str, sizeof(level_str));

    if (!tag[0]) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing tag");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    bb_log_level_t level;
    if (!bb_log_level_from_str(level_str, &level)) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid level");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    bb_log_level_set(tag, level);
    bb_log_d(TAG, "set log level: tag=%s level=%s", tag, level_str);

    bb_http_resp_set_status(req, 204);
    bb_err_t err = bb_http_resp_send_chunk(req, NULL, 0);
    return err;
}

static bb_err_t log_level_get_handler(bb_http_request_t *req)
{
    // Streaming JSON: {"levels":[...], "tags":[...]}
    static const char *s_level_names[] = {
        "none", "error", "warn", "info", "debug", "verbose",
    };

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    // "levels" array
    bb_http_resp_json_obj_set_arr_begin(&obj, "levels");
    for (size_t i = 0; i < sizeof(s_level_names) / sizeof(s_level_names[0]); i++) {
        bb_http_resp_json_obj_set_str(&obj, NULL, s_level_names[i]);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    // "tags" array of objects
    bb_http_resp_json_obj_set_arr_begin(&obj, "tags");
    const char *tag = NULL;
    bb_log_level_t lv;
    for (size_t i = 0; bb_log_tag_at(i, &tag, &lv); i++) {
        bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
        bb_http_resp_json_obj_set_str(&obj, "tag",   tag);
        bb_http_resp_json_obj_set_str(&obj, "level", bb_log_level_to_str(lv));
        bb_http_resp_json_obj_set_obj_end(&obj);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_log_level_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"levels\":{\"type\":\"array\","
      "\"items\":{\"type\":\"string\","
      "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}},"
      "\"tags\":{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"tag\":{\"type\":\"string\"},"
      "\"level\":{\"type\":\"string\"}},"
      "\"required\":[\"tag\",\"level\"]}}},"
      "\"required\":[\"levels\",\"tags\"]}",
      "current per-tag log levels and available level names" },
    { 0 },
};

static const bb_route_t s_log_level_get_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/log/level",
    .tag      = "logs",
    .summary  = "Get log levels",
    .responses = s_log_level_get_responses,
    .handler  = log_level_get_handler,
};

static const bb_route_response_t s_log_level_post_responses[] = {
    { 204, NULL, NULL, "log level updated" },
    { 400, "text/plain", NULL, "invalid tag or level" },
    { 0 },
};

static const bb_route_t s_log_level_post_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/log/level",
    .tag                  = "logs",
    .summary              = "Set log level for a tag",
    .request_content_type = "application/x-www-form-urlencoded",
    .request_schema       =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"tag\":{\"type\":\"string\"},"
        "\"level\":{\"type\":\"string\","
        "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}},"
        "\"required\":[\"tag\",\"level\"]}",
    .responses = s_log_level_post_responses,
    .handler  = log_level_handler,
};

static bb_err_t bb_log_register_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_log_level_post_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_log_level_get_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "log level routes registered");
    return BB_OK;
}

// PRE_HTTP companion: declare route count before server starts (must match
// the number of bb_http_register_* calls in bb_log_register_routes_init: 2).
static bb_err_t bb_log_register_routes_reserve(void)
{
    bb_http_reserve_routes(2);  // POST /api/log/level + GET /api/log/level
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_log_register_routes, bb_log_register_routes_reserve);
BB_REGISTRY_REGISTER_N(bb_log_register_routes, bb_log_register_routes_init, 4);
