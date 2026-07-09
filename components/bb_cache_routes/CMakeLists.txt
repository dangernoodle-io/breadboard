idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/cache_route_status.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache_routes/bb_cache_routes.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_cache
    PRIV_REQUIRES bb_log bb_http bb_http_server)
