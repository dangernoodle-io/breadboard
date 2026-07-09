idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_power_routes/bb_power_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_power bb_core bb_json bb_http_server
    PRIV_REQUIRES bb_log)
