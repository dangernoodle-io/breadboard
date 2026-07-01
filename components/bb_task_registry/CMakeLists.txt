idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_task_registry/bb_task_registry.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_task_registry/bb_task_registry_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_registry bb_wdt bb_init esp_system)

if(CONFIG_BB_TASK_REGISTRY_SW_WDT)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_task_registry_sw_wdt)
endif()
