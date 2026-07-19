# bb_serialize: PUBLIC -- include/bb_ota_check_wire.h (the "update.available"
# wire descriptor, B1-1045 PR-2) includes bb_serialize.h.
idf_component_register(
    SRCS "src/bb_ota_check_common.c"
         "src/bb_ota_check_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_check/bb_ota_check_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_release_manifest bb_serialize
    PRIV_REQUIRES bb_http_client bb_data bb_http bb_http_server bb_log bb_mdns bb_settings bb_openapi bb_system bb_json esp_http_server bb_timer esp_hw_support bb_cache bb_task bb_str)
