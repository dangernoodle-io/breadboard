cmake_minimum_required(VERSION 3.16.0)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")
# bb_display depends on espressif/esp_lcd_ek79007 (P4-only) — exclude on classic ESP32.
set(EXCLUDE_COMPONENTS bb_display)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(minimal)
idf_build_set_property(COMPILE_OPTIONS "-DFIRMWARE_BOARD_esp32_wroom_32=1" APPEND)
