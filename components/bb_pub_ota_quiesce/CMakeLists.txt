idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_ota_quiesce/bb_pub_ota_quiesce.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_pub bb_update_check bb_ota_hooks bb_log bb_registry)

if(CONFIG_BB_PUB_QUIESCE_ON_OTA)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub_ota_quiesce)
endif()
