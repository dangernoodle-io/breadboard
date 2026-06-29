idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_rtos/bb_pub_rtos.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub
    PRIV_REQUIRES bb_json bb_log bb_openapi bb_registry)

if(CONFIG_BB_PUB_RTOS_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub_rtos)
endif()
