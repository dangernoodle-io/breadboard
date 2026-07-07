idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_power_routes/bb_power_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_power bb_core bb_json
    PRIV_REQUIRES bb_log bb_init bb_http_server)

if(CONFIG_BB_POWER_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_power_routes)
endif()
