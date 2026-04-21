idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_info/bb_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_nv bb_board bb_wifi bb_json esp_http_server
)
