idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_pull/bb_ota_pull.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_wifi esp_https_ota esp_http_client app_update
             esp_app_format esp-tls mbedtls bb_log bb_json
    PRIV_REQUIRES bb_registry
)

if(CONFIG_BB_OTA_PULL_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_pull)
endif()
