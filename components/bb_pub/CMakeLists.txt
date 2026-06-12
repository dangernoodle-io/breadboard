idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub/bb_pub.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_pub/bb_pub_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_nv bb_log bb_timer bb_registry esp_timer)

if(CONFIG_BB_PUB_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub)
endif()
