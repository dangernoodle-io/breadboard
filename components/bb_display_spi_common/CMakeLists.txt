idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_spi_common/bb_display_spi_common.c"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_spi_common"
    REQUIRES bb_core esp_lcd esp_driver_spi
    PRIV_REQUIRES bb_log)
