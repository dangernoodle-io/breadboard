idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_manifest_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_manifest/bb_manifest_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_log bb_init bb_http bb_http_server
)

if(CONFIG_BB_MANIFEST_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_manifest)
    bb_init_force_register(${COMPONENT_LIB} bb_manifest)
endif()
