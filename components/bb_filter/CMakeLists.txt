idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_filter.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_attrs
)
