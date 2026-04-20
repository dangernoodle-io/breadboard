idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/ota_pull/ota_pull.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_https_ota esp_http_client app_update
             esp_app_format json esp-tls mbedtls log_stream
)
