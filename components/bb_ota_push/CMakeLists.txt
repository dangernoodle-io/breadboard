idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_push/bb_ota_push.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http app_update esp_app_format bb_log
)
