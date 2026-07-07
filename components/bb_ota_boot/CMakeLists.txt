idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_boot/bb_ota_boot.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server
    PRIV_REQUIRES bb_tls bb_log bb_init bb_nv bb_ntp bb_wifi bb_board bb_ota_pull bb_ota_hooks bb_ota_check bb_http_client bb_task bb_system esp_system heap freertos espressif__mdns bb_str
)

if(CONFIG_BB_OTA_BOOT_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_ota_boot)
    bb_init_force_register(${COMPONENT_LIB} bb_ota_boot)
endif()

# bb_ota_boot has no platform/host/bb_ota_boot/ — its impl is portable
# (guarded by #ifdef ESP_PLATFORM internally); the host build needs it too.
# bbtool-scaffold-hint: source=platform/espidf/bb_ota_boot/bb_ota_boot.c
