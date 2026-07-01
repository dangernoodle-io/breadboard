idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_task_registry/bb_task_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_registry bb_wdt esp_system)
