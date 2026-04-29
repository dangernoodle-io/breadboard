idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display/bb_display.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core log esp_lcd esp_driver_gpio esp_lvgl_port
    PRIV_REQUIRES bb_log bb_hw)
