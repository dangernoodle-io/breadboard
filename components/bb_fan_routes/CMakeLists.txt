idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_fan_routes/bb_fan_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_fan bb_core
    PRIV_REQUIRES bb_http bb_log bb_registry bb_json)

if(CONFIG_BB_FAN_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_fan_routes)
endif()
