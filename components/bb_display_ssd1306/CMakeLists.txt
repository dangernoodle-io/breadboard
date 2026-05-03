idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_ssd1306/bb_display_ssd1306.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_display esp_lcd esp_driver_i2c
    PRIV_REQUIRES bb_log bb_hw nvs_flash)

# Pin the constructor under PlatformIO's --whole-archive-stripping linker.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bb_display_register__ssd1306")
