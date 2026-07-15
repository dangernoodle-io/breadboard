idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_http/bb_wifi_http_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi_http/bb_wifi_http_emit.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_wifi bb_json
    PRIV_REQUIRES bb_cache bb_http bb_http_server bb_openapi esp_wifi
)
