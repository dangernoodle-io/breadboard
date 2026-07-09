idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_temp/bb_temp.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_health bb_json bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_system)
