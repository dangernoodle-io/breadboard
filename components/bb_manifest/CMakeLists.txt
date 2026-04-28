idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_manifest_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_manifest/bb_manifest_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http bb_json bb_log
    PRIV_REQUIRES bb_registry
)

if(CONFIG_BB_MANIFEST_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_manifest)
endif()
