idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_init/bb_init.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
)
