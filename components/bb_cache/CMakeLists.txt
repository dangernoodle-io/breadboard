idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache/bb_cache_espidf.c"
         "src/bb_cache_evaluate.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_event
    PRIV_REQUIRES bb_log bb_str)
