idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_boot/bb_ota_boot.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http
    PRIV_REQUIRES bb_log bb_registry bb_nv bb_ntp bb_wifi bb_ota_pull bb_ota_hooks bb_update_check esp_system espressif__mdns
)

if(CONFIG_BB_OTA_BOOT_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_ota_boot)
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_boot)
endif()
