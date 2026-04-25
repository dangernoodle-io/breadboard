#include "bb_board.h"

#include <string.h>

#include "bb_json.h"
#include "esp_err.h"
#include "esp_http_server.h"

static esp_err_t board_info_handler(httpd_req_t *req)
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
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    if (json) bb_json_free_str(json);
    bb_json_free(root);
    return err;
}

bb_err_t bb_board_register_routes(void *server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    httpd_handle_t h = (httpd_handle_t)server;
    httpd_uri_t uri = {
        .uri = "/api/board", .method = HTTP_GET, .handler = board_info_handler, .user_ctx = NULL,
    };
    return httpd_register_uri_handler(h, &uri);
}
