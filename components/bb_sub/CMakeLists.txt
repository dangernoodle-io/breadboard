idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_sub.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_event
    PRIV_REQUIRES bb_cache bb_json bb_log)
