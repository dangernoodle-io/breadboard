#include "bb_log.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_log_http";

static bb_err_t log_level_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        return bb_http_resp_send_err(req, 400, "invalid request");
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return bb_http_resp_send_err(req, 400, "read failed");
    }
    body[n] = '\0';

    char tag[64];
    char level_str[16];
    bb_url_decode_field(body, "tag", tag, sizeof(tag));
    bb_url_decode_field(body, "level", level_str, sizeof(level_str));

    if (!tag[0]) {
        return bb_http_resp_send_err(req, 400, "missing tag");
    }

    bb_log_level_t level;
    if (!bb_log_level_from_str(level_str, &level)) {
        return bb_http_resp_send_err(req, 400, "invalid level");
    }

    bb_log_level_set(tag, level);
    bb_log_d(TAG, "set log level: tag=%s level=%s", tag, level_str);

    bb_http_resp_set_status(req, 204);
    return bb_http_resp_send(req, NULL, 0);
}

static bb_err_t log_level_get_handler(bb_http_request_t *req)
{
    // Build JSON response: {"levels":["none",...], "tags":[{"tag":"x","level":"y"},...]}
    bb_json_t root = bb_json_obj_new();
    if (!root) {
        return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    }

    // Levels array
    bb_json_t levels_arr = bb_json_arr_new();
    if (!levels_arr) {
        bb_json_free(root);
        return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    }
    bb_json_arr_append_string(levels_arr, "none");
    bb_json_arr_append_string(levels_arr, "error");
    bb_json_arr_append_string(levels_arr, "warn");
    bb_json_arr_append_string(levels_arr, "info");
    bb_json_arr_append_string(levels_arr, "debug");
    bb_json_arr_append_string(levels_arr, "verbose");
    bb_json_obj_set_arr(root, "levels", levels_arr);

    // Tags array
    bb_json_t tags_arr = bb_json_arr_new();
    if (!tags_arr) {
        bb_json_free(root);
        return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    }

    const char *tag = NULL;
    bb_log_level_t level;
    for (size_t i = 0; bb_log_tag_at(i, &tag, &level); i++) {
        bb_json_t tag_obj = bb_json_obj_new();
        if (!tag_obj) {
            bb_json_free(tags_arr);
            bb_json_free(root);
            return bb_http_resp_send_err(req, 500, "JSON alloc failed");
        }
        bb_json_obj_set_string(tag_obj, "tag", tag);
        bb_json_obj_set_string(tag_obj, "level", bb_log_level_to_str(level));
        bb_json_arr_append_obj(tags_arr, tag_obj);
    }

    bb_json_obj_set_arr(root, "tags", tags_arr);

    // Serialize
    char *json_str = bb_json_serialize(root);
    bb_json_free(root);

    if (!json_str) {
        return bb_http_resp_send_err(req, 500, "JSON serialize failed");
    }

    bb_http_resp_set_status(req, 200);
    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_send(req, json_str, strlen(json_str));
    bb_json_free_str(json_str);
    return err;
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

BB_REGISTRY_REGISTER_N(bb_log_register_routes, bb_log_register_routes_init, 2);
