idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_openapi/bb_openapi_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http bb_json bb_log bb_system
)
