idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../platform/espidf/bb_display_ili9341/bb_display_ili9341.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_display
    PRIV_REQUIRES bb_log bb_display_spi_common nvs_flash esp_lcd esp_driver_spi esp_driver_gpio)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../platform/espidf/bb_display_spi_common")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bb_display_register__ili9341")
