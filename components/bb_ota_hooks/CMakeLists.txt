idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_hooks/bb_ota_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_ota_hooks/bb_ota_led.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_event bb_event_routes bb_openapi bb_registry
)

if(CONFIG_BB_OTA_HOOKS_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_hooks)
endif()
