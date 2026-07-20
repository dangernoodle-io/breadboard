idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/bb_sensors_wire.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_sensors_dispatch.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sensors/bb_sensors.c"
    INCLUDE_DIRS "include" "."
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_fan bb_power bb_thermal bb_data bb_serialize
)
