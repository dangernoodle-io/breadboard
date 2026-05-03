set(_st77xx_srcs
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_st77xx/bb_display_st77xx_common.c")

if(CONFIG_BB_DISPLAY_ST77XX_VARIANT_ST7735)
    list(APPEND _st77xx_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_st77xx/bb_display_st7735.c")
endif()
if(CONFIG_BB_DISPLAY_ST77XX_VARIANT_ST7789)
    list(APPEND _st77xx_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_st77xx/bb_display_st7789.c")
endif()

idf_component_register(
    SRCS ${_st77xx_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_display esp_lcd esp_driver_spi esp_driver_gpio
    PRIV_REQUIRES bb_log bb_hw)

# Force-keep the constructor for whichever variant is selected.
if(CONFIG_BB_DISPLAY_ST77XX_VARIANT_ST7735)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bb_display_register__st7735")
endif()
if(CONFIG_BB_DISPLAY_ST77XX_VARIANT_ST7789)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bb_display_register__st7789")
endif()
