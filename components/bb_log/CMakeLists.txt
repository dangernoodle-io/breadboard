idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf freertos log bb_http esp_http_server
)
