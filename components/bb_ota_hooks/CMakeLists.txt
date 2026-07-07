idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_hooks/bb_ota_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_ota_hooks/bb_ota_led.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_event bb_event_routes bb_openapi bb_init bb_http_server
)

if(CONFIG_BB_OTA_HOOKS_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_ota_hooks)
endif()

# bb_ota_hooks.c is portable (guarded by #ifdef ESP_PLATFORM internally) but
# filed under platform/espidf/ only; the host build needs it alongside the
# existing platform/host/bb_ota_hooks/bb_ota_led.c.
# bbtool-scaffold-hint: source=platform/espidf/bb_ota_hooks/bb_ota_hooks.c
