idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_board/bb_board.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_board/bb_board_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_http app_update spi_flash esp_hw_support esp_app_format bb_str bb_meminfo
)
