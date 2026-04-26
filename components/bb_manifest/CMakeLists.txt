idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_manifest_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_manifest/bb_manifest_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http bb_json bb_log esp_http_server
)
