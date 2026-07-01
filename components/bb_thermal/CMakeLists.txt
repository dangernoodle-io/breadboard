idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_thermal/bb_thermal.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_temp bb_power bb_fan bb_core bb_json
    PRIV_REQUIRES bb_log bb_init)

if(CONFIG_BB_THERMAL_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_thermal)
endif()
