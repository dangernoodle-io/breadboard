idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pool/bb_pool.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_mem_arena
    PRIV_REQUIRES bb_log bb_str
)
