idf_component_register(
    SRCS "src/display.c"
    INCLUDE_DIRS "include"
    REQUIRES board log log_stream esp_lcd esp_driver_gpio esp_lvgl_port)
