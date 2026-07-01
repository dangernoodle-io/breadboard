idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_validate.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_openapi/bb_openapi_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_json
    PRIV_REQUIRES bb_log bb_system bb_init json
)

if(CONFIG_BB_OPENAPI_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_openapi)
    bb_init_force_register(${COMPONENT_LIB} bb_openapi)
endif()
