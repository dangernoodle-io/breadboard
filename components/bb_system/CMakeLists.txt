idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_log bb_nv
)
