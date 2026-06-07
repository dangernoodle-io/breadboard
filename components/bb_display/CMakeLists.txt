set(SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display.c")
set(_display_reqs bb_core)
set(_display_priv_reqs bb_log)

if(CONFIG_BB_DISPLAY_FONT_8X16)
    list(APPEND SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_8x16.c")
endif()
if(CONFIG_BB_DISPLAY_FONT_6X12)
    list(APPEND SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_6x12.c")
endif()
if(CONFIG_BB_DISPLAY_FONT_5X8)
    list(APPEND SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_5x8.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${_display_reqs}
    PRIV_REQUIRES ${_display_priv_reqs})
