idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wdt/bb_wdt.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_wdt_park_wait.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_wdt_core_claim.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log esp_system freertos
)
