cmake_minimum_required(VERSION 3.16.0)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")
# bb_display depends on espressif/esp_lcd_ek79007 (P4-only) — exclude on classic ESP32.
set(EXCLUDE_COMPONENTS bb_display)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(minimal)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/board" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DBB_HW_BOARD_HEADER=\"esp32_wroom_32.h\"" APPEND)
