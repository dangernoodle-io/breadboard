idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_info/bb_info.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config bb_board bb_wifi esp_http_server json
)
