idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub/bb_pub.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_pub/bb_pub_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_cache
    PRIV_REQUIRES bb_nv bb_settings bb_log bb_pool bb_mem_arena bb_ntp bb_timer bb_str)
