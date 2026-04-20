idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/display/display.c"
    INCLUDE_DIRS "include"
    REQUIRES board log log_stream esp_lcd esp_driver_gpio esp_lvgl_port)
