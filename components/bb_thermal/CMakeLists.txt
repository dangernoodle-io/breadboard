idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_thermal/bb_thermal.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_temp bb_power bb_fan bb_core bb_http_server
    PRIV_REQUIRES bb_log)
