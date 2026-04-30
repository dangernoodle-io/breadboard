cmake_minimum_required(VERSION 3.16.0)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")

if(NOT DEFINED FIRMWARE_BOARD)
    message(FATAL_ERROR "FIRMWARE_BOARD must be set via -DFIRMWARE_BOARD=...")
endif()

if(FIRMWARE_BOARD STREQUAL "esp32_wroom_32")
    # bb_display depends on espressif/esp_lcd_ek79007 (P4-only) — exclude on classic ESP32.
    set(EXCLUDE_COMPONENTS bb_display)
    set(BB_BOARD_HEADER "esp32_wroom_32.h")
elseif(FIRMWARE_BOARD STREQUAL "esp32_c3_devkitm_1")
    set(EXCLUDE_COMPONENTS bb_display)
    set(BB_BOARD_HEADER "esp32_c3_devkitm_1.h")
elseif(FIRMWARE_BOARD STREQUAL "elecrow_p4_hmi7")
    set(BB_BOARD_HEADER "elecrow_p4_hmi7.h")
else()
    message(FATAL_ERROR "Unknown FIRMWARE_BOARD: ${FIRMWARE_BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smoke)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/board" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DBB_HW_BOARD_HEADER=\"${BB_BOARD_HEADER}\"" APPEND)
