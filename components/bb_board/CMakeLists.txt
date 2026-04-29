idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_board/bb_board.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_board/bb_board_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv
    PRIV_REQUIRES bb_registry bb_http bb_json app_update spi_flash esp_hw_support esp_app_format
)

if(CONFIG_BB_BOARD_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_board)
endif()
