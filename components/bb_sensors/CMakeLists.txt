idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sensors/bb_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_section
    PRIV_REQUIRES bb_http bb_log bb_init bb_fan_routes bb_power_routes bb_thermal bb_fan)

if(CONFIG_BB_SENSORS_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_sensors)
endif()
