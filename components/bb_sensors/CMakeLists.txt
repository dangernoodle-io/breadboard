idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sensors/bb_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_response bb_http_server
    PRIV_REQUIRES bb_http bb_log bb_thermal bb_fan bb_power)
