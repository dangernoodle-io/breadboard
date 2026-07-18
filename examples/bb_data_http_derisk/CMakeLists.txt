cmake_minimum_required(VERSION 3.16.0)

# THROWAWAY DE-RISK EXAMPLE (B1-1033 first bench-flash de-risk, design
# KB 1447) -- proves the bb_data_http ESP-IDF backend
# (platform/espidf/bb_data_http/bb_data_http_espidf.c) on real hardware
# BEFORE the B1-1045 atomic egress cutover. Hand-wired app_main (no codegen,
# no bb_init walker), a single TEST binding, and a TEMP route -- this whole
# example directory is DELETED at cutover, not evolved into anything.
#
# Usage (never `pio ci`):
#   pio run -d . -e esp32

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components"
)

# Without an explicit COMPONENTS allowlist, ESP-IDF builds every component
# discovered under EXTRA_COMPONENT_DIRS -- including the display drivers,
# whose pin/geometry defines (PIN_LCD_CLK, LCD_WIDTH, ...) come from a
# board header this hand-wired, display-less example never supplies (see
# examples/smoke's -DBB_HW_BOARD_HEADER wiring). This example touches no
# display, so every display backend is excluded outright, same as any
# smoke board that has none.
set(EXCLUDE_COMPONENTS bb_display_ek79007 bb_display_st77xx bb_display_ssd1306 bb_display_ili9341)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bb_data_http_derisk)

# bb_hw.h (bb_core) hard-requires BB_HW_BOARD_HEADER -- transitively pulled
# in by bb_mdns (bb_wifi's own closure), not just anything display-related.
# Mirrors examples/smoke's own board-header wiring, minus the per-board
# matrix (this example has exactly one board: the WROOM-32 pogopin bench).
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/board" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DBB_HW_BOARD_HEADER=\"esp32_wroom_32.h\"" APPEND)
