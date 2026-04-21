idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_board/bb_board.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_board/bb_board_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config esp_app_format esp_system app_update esp_http_server json esp_hw_support spi_flash
)
