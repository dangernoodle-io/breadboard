idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/ota_push/ota_push.c"
    INCLUDE_DIRS "include"
    REQUIRES http_server esp_http_server app_update esp_app_format log_stream
)
