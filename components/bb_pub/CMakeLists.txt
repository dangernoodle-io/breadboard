idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub/bb_pub.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_pub/bb_pub_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_cache
    PRIV_REQUIRES bb_nv bb_log bb_pool bb_arena bb_ntp bb_timer bb_init)

if(CONFIG_BB_PUB_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_pub)
endif()
