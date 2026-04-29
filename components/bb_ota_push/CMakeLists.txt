idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_push/bb_ota_push.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http
    PRIV_REQUIRES bb_registry bb_log app_update esp_app_format
)

if(CONFIG_BB_OTA_PUSH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_push)
endif()
