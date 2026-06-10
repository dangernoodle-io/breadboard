idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_push/bb_ota_push.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_nv
    PRIV_REQUIRES bb_wdt bb_registry bb_log bb_ota_hooks app_update esp_app_format esp_timer
)

if(CONFIG_BB_OTA_PUSH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_ota_push)
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_push)
endif()
