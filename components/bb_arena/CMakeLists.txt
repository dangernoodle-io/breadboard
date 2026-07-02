idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_arena/bb_arena.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
)
