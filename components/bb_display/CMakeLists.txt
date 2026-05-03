idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_display.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_8x16.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_6x8.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_5x7.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log)
