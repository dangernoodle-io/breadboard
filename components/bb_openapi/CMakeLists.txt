idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_openapi/bb_openapi_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http bb_json bb_log bb_system
    PRIV_REQUIRES bb_registry
)

if(CONFIG_BB_OPENAPI_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_openapi)
endif()
