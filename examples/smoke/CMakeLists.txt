cmake_minimum_required(VERSION 3.16.0)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/bb_component_dirs.cmake")
bb_component_dirs("${CMAKE_CURRENT_SOURCE_DIR}/../../components" BB_COMPONENT_DIRS)
list(APPEND EXTRA_COMPONENT_DIRS
    ${BB_COMPONENT_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/espidf/bb_queue_espidf"
)

if(NOT DEFINED FIRMWARE_BOARD)
    message(FATAL_ERROR "FIRMWARE_BOARD must be set via -DFIRMWARE_BOARD=...")
endif()

if(FIRMWARE_BOARD STREQUAL "esp32_wroom_32")
    set(EXCLUDE_COMPONENTS bb_display_ek79007 bb_display_st77xx bb_display_ssd1306 bb_display_ili9341)
    set(BB_BOARD_HEADER "esp32_wroom_32.h")
elseif(FIRMWARE_BOARD STREQUAL "esp32_c3_devkitm_1")
    set(EXCLUDE_COMPONENTS bb_display_ek79007 bb_display_st77xx bb_display_ssd1306 bb_display_ili9341)
    set(BB_BOARD_HEADER "esp32_c3_devkitm_1.h")
elseif(FIRMWARE_BOARD STREQUAL "lilygo_t_dongle_s3")
    set(EXCLUDE_COMPONENTS bb_display_ek79007 bb_display_ssd1306 bb_display_ili9341)
    set(BB_BOARD_HEADER "lilygo_t_dongle_s3.h")
elseif(FIRMWARE_BOARD STREQUAL "elecrow_p4_hmi7")
    set(EXCLUDE_COMPONENTS bb_display_st77xx bb_display_ssd1306 bb_display_ili9341)
    set(BB_BOARD_HEADER "elecrow_p4_hmi7.h")
else()
    message(FATAL_ERROR "Unknown FIRMWARE_BOARD: ${FIRMWARE_BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smoke)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/board" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DBB_HW_BOARD_HEADER=\"${BB_BOARD_HEADER}\"" APPEND)

# Apply -Werror to bb_* components only — not to ESP-IDF system components or
# managed_components (espressif__mdns, etc.).  idf_component_get_property
# returns the CMake library target; target_compile_options is PRIVATE so
# downstream consumers are not infected.
idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(comp ${build_components})
    if("${comp}" MATCHES "^bb_")
        idf_component_get_property(comp_lib "${comp}" COMPONENT_LIB)
        if(comp_lib)
            get_target_property(_bb_lib_type "${comp_lib}" TYPE)
            if(NOT _bb_lib_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options("${comp_lib}" PRIVATE -Werror)
            endif()
        endif()
    endif()
endforeach()
