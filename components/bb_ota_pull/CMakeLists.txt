idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_pull/bb_ota_pull.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server
    PRIV_REQUIRES bb_tls bb_wdt bb_wifi bb_meminfo bb_log bb_release_manifest bb_http_client bb_ota_check bb_ota_hooks bb_task bb_system esp_https_ota esp_http_client app_update esp_app_format esp-tls mbedtls esp_timer bb_str
)

# bb_ota_pull.c is portable (guarded by #ifdef ESP_PLATFORM internally) but
# filed under platform/espidf/ only (platform/host/bb_ota_pull/ holds just a
# test-hook header); the host build needs it too.
# bbtool-scaffold-hint: source=platform/espidf/bb_ota_pull/bb_ota_pull.c
