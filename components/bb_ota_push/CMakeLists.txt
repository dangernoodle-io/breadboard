idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_push/bb_ota_push.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http app_update esp_app_format bb_log
    PRIV_REQUIRES bb_registry
)

if(CONFIG_BB_OTA_PUSH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_push)
endif()
