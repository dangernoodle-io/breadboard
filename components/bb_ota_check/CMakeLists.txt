idf_component_register(
    SRCS "src/bb_ota_check_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_check/bb_ota_check_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_release_manifest
    PRIV_REQUIRES bb_alert bb_http_client bb_event bb_http bb_log bb_mdns bb_nv bb_openapi bb_system bb_init bb_json esp_http_server bb_event_routes bb_timer esp_hw_support bb_cache bb_task_registry)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_OTA_CHECK_AUTOREGISTER)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_ota_check)
    bb_init_force_register(${COMPONENT_LIB} bb_ota_check)
endif()
