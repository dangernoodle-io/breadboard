idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ws_server/bb_ws_server.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ws_server/bb_ws_server_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server
    PRIV_REQUIRES bb_log esp_http_server bb_str
)
