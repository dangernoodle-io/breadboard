idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/http_server/http_server.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/system/system.c"
         "src/http_utils.c"
         "src/common_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config log_stream bb_wifi esp_http_server esp_wifi esp_netif esp_app_format json
)
