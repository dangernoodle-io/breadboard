idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_pull/bb_ota_pull.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http
    PRIV_REQUIRES bb_tls bb_wdt bb_wifi bb_board bb_log bb_json bb_init bb_release_manifest bb_http_client bb_ota_check bb_ota_hooks bb_task_registry bb_system esp_https_ota esp_http_client app_update esp_app_format esp-tls mbedtls esp_timer
)

if(CONFIG_BB_OTA_PULL_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_ota_pull)
    bb_init_force_register(${COMPONENT_LIB} bb_ota_pull)
endif()
