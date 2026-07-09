idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_boot/bb_ota_boot.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server
    PRIV_REQUIRES bb_tls bb_log bb_nv bb_ntp bb_wifi bb_board bb_ota_pull bb_ota_hooks bb_ota_check bb_http_client bb_task bb_system esp_system heap freertos espressif__mdns bb_str
)

# bb_ota_boot has no platform/host/bb_ota_boot/ — its impl is portable
# (guarded by #ifdef ESP_PLATFORM internally); the host build needs it too.
# bbtool-scaffold-hint: source=platform/espidf/bb_ota_boot/bb_ota_boot.c
