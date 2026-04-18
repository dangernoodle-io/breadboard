idf_component_register(
    SRCS "src/display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd board
    PRIV_REQUIRES esp_lcd_mipi_dsi)
