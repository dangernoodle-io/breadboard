idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_timer/bb_timer.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log esp_timer bb_task bb_wdt
    KCONFIG "${CMAKE_CURRENT_LIST_DIR}/Kconfig"
)
