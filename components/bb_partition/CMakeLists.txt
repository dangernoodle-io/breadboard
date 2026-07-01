idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_partition/bb_partition.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_partition/bb_partition_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log esp_partition app_update bb_http bb_json bb_init)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_PARTITION_ROUTES_AUTOREGISTER)
    bb_init_force_register(${COMPONENT_LIB} bb_partition_routes)
endif()
