#include "bb_board.h"

#include <string.h>

#include "bb_http.h"
#include "bb_registry.h"

static bb_err_t board_info_handler(bb_http_request_t *req)
{
    bb_board_info_t info;
    bb_board_get_info(&info);

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj, "board",        info.board);
    bb_http_resp_json_obj_set_str(&obj, "project_name", info.project_name);
    bb_http_resp_json_obj_set_str(&obj, "version",      info.version);
    bb_http_resp_json_obj_set_str(&obj, "idf_version",  info.idf_version);
    bb_http_resp_json_obj_set_str(&obj, "build_date",   info.build_date);
    bb_http_resp_json_obj_set_str(&obj, "build_time",   info.build_time);
    bb_http_resp_json_obj_set_str(&obj, "chip_model",   info.chip_model);
    bb_http_resp_json_obj_set_int(&obj, "cores",        (int64_t)info.cores);
    bb_http_resp_json_obj_set_str(&obj, "mac",          info.mac);
    bb_http_resp_json_obj_set_int(&obj, "flash_size",   (int64_t)info.flash_size);
    bb_http_resp_json_obj_set_int(&obj, "total_heap",   (int64_t)info.total_heap);
    bb_http_resp_json_obj_set_int(&obj, "free_heap",    (int64_t)info.free_heap);
    bb_http_resp_json_obj_set_int(&obj, "app_size",     (int64_t)info.app_size);
    bb_http_resp_json_obj_set_str(&obj, "reset_reason", info.reset_reason);
    bb_http_resp_json_obj_set_bool(&obj, "ota_validated", info.ota_validated);
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_board_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"board\":{\"type\":\"string\"},"
      "\"project_name\":{\"type\":\"string\"},"
      "\"version\":{\"type\":\"string\"},"
      "\"idf_version\":{\"type\":\"string\"},"
      "\"build_date\":{\"type\":\"string\"},"
      "\"build_time\":{\"type\":\"string\"},"
      "\"chip_model\":{\"type\":\"string\"},"
      "\"cores\":{\"type\":\"integer\"},"
      "\"mac\":{\"type\":\"string\"},"
      "\"flash_size\":{\"type\":\"integer\"},"
      "\"total_heap\":{\"type\":\"integer\"},"
      "\"free_heap\":{\"type\":\"integer\"},"
      "\"app_size\":{\"type\":\"integer\"},"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"ota_validated\":{\"type\":\"boolean\"}},"
      "\"required\":[\"board\",\"version\"]}",
      "board hardware and firmware info" },
    { 0 },
};

static const bb_route_t s_board_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/board",
    .tag      = "board",
    .summary  = "Get board info",
    .responses = s_board_responses,
    .handler  = board_info_handler,
};

static bb_err_t bb_board_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_http_register_described_route(server, &s_board_route);
    if (err != BB_OK) return err;
    return BB_OK;
}

#if CONFIG_BB_BOARD_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_board, bb_board_init, 1);
#endif
