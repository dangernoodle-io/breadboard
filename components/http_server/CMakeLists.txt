idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/http_server/http_server.c"
         "src/http_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config log_stream esp_http_server esp_wifi esp_netif
)
