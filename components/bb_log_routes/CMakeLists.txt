idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_routes/bb_log_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_routes/bb_log_http.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_log bb_nv bb_http bb_json
    PRIV_REQUIRES bb_registry
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
bb_registry_force_register(${COMPONENT_LIB} bb_log_register_routes)
bb_registry_force_register(${COMPONENT_LIB} bb_log_stream_register_routes)
