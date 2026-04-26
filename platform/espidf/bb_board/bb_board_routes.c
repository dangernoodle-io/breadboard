#include "bb_board.h"

#include <string.h>

#include "bb_http.h"
#include "bb_json.h"

static bb_err_t board_info_handler(bb_http_request_t *req)
{
    bb_board_info_t info;
    bb_board_get_info(&info);

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "board", info.board);
    bb_json_obj_set_string(root, "project_name", info.project_name);
    bb_json_obj_set_string(root, "version", info.version);
    bb_json_obj_set_string(root, "idf_version", info.idf_version);
    bb_json_obj_set_string(root, "build_date", info.build_date);
    bb_json_obj_set_string(root, "build_time", info.build_time);
    bb_json_obj_set_string(root, "chip_model", info.chip_model);
    bb_json_obj_set_number(root, "cores", (double)info.cores);
    bb_json_obj_set_string(root, "mac", info.mac);
    bb_json_obj_set_number(root, "flash_size", (double)info.flash_size);
    bb_json_obj_set_number(root, "total_heap", (double)info.total_heap);
    bb_json_obj_set_number(root, "free_heap", (double)info.free_heap);
    bb_json_obj_set_number(root, "app_size", (double)info.app_size);
    bb_json_obj_set_string(root, "reset_reason", info.reset_reason);
    bb_json_obj_set_bool(root, "ota_validated", info.ota_validated);

    char *json = bb_json_serialize(root);
    bb_http_resp_set_header(req, "Content-Type", "application/json");
    bb_err_t err = bb_http_resp_send(req, json ? json : "{}", json ? strlen(json) : 2);
    if (json) bb_json_free_str(json);
    bb_json_free(root);
    return err;
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

bb_err_t bb_board_register_routes(void *server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    return bb_http_register_described_route(server, &s_board_route);
}
