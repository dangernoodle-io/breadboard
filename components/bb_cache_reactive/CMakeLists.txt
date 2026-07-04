idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache_reactive/bb_cache_reactive_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_cache
    PRIV_REQUIRES bb_json bb_log)
