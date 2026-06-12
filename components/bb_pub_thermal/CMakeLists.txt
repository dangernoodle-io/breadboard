idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_thermal/bb_pub_thermal.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub bb_fan bb_power bb_temp
    PRIV_REQUIRES bb_json bb_log bb_registry)

if(CONFIG_BB_PUB_THERMAL_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub_thermal)
endif()
