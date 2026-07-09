idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_push/bb_ota_push.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server bb_nv
    PRIV_REQUIRES bb_wdt bb_log bb_ota_hooks bb_system app_update esp_app_format
)

# bb_ota_push has no platform/host/bb_ota_push/ — its impl is portable
# (guarded by #ifdef ESP_PLATFORM internally); the host build needs it too.
# bbtool-scaffold-hint: source=platform/espidf/bb_ota_push/bb_ota_push.c
