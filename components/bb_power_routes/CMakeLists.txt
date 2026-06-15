idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_power_routes/bb_power_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_power bb_core bb_json
    PRIV_REQUIRES bb_http bb_log bb_registry)

if(CONFIG_BB_POWER_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_power_routes)
endif()
