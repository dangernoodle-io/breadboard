idf_component_register(
    SRCS "src/bb_ota_check_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_check/bb_ota_check_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_release_manifest
    PRIV_REQUIRES bb_http_client bb_event bb_http bb_http_server bb_log bb_mdns bb_nv bb_openapi bb_system bb_json esp_http_server bb_event_routes bb_timer esp_hw_support bb_cache bb_task bb_str)
