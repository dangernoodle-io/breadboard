#include "bb_board.h"

#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

static esp_err_t board_info_handler(httpd_req_t *req)
{
    bb_board_info_t info;
    bb_board_get_info(&info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "board", info.board);
    cJSON_AddStringToObject(root, "project_name", info.project_name);
    cJSON_AddStringToObject(root, "version", info.version);
    cJSON_AddStringToObject(root, "idf_version", info.idf_version);
    cJSON_AddStringToObject(root, "build_date", info.build_date);
    cJSON_AddStringToObject(root, "build_time", info.build_time);
    cJSON_AddStringToObject(root, "chip_model", info.chip_model);
    cJSON_AddNumberToObject(root, "cores", (double)info.cores);
    cJSON_AddStringToObject(root, "mac", info.mac);
    cJSON_AddNumberToObject(root, "flash_size", (double)info.flash_size);
    cJSON_AddNumberToObject(root, "total_heap", (double)info.total_heap);
    cJSON_AddNumberToObject(root, "free_heap", (double)info.free_heap);
    cJSON_AddNumberToObject(root, "app_size", (double)info.app_size);
    cJSON_AddStringToObject(root, "reset_reason", info.reset_reason);
    cJSON_AddBoolToObject(root, "ota_validated", info.ota_validated);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    if (json) cJSON_free(json);
    cJSON_Delete(root);
    return err;
}

esp_err_t bb_board_register_routes(void *server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    httpd_handle_t h = (httpd_handle_t)server;
    httpd_uri_t uri = {
        .uri = "/api/board", .method = HTTP_GET, .handler = board_info_handler, .user_ctx = NULL,
    };
    return httpd_register_uri_handler(h, &uri);
}
