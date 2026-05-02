idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_ek79007/bb_display_ek79007.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_display esp_lcd esp_driver_gpio esp_lvgl_port
    PRIV_REQUIRES bb_log bb_hw)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bb_display_register__ek79007")
