idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/cache_route_status.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache_routes/bb_cache_routes.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_cache
    PRIV_REQUIRES bb_log bb_http bb_init)

if(CONFIG_BB_CACHE_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_cache_routes)
endif()
