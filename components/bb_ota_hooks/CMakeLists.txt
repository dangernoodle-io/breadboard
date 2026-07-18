idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_hooks/bb_ota_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_ota_hooks/bb_ota_led.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_ota_hooks_wire.c"
    INCLUDE_DIRS "include"
    # bb_serialize: PUBLIC -- include/bb_ota_hooks_wire.h (the "ota.progress"
    # wire descriptor, B1-1045 PR-2) includes bb_serialize.h.
    REQUIRES bb_core bb_serialize
    PRIV_REQUIRES bb_log bb_event bb_event_routes bb_openapi bb_http_server bb_str
)

# bb_ota_hooks.c is portable (guarded by #ifdef ESP_PLATFORM internally) but
# filed under platform/espidf/ only; the host build needs it alongside the
# existing platform/host/bb_ota_hooks/bb_ota_led.c.
# bbtool-scaffold-hint: source=platform/espidf/bb_ota_hooks/bb_ota_hooks.c
