idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/log_stream/log_stream.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/log_stream/log_stream_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf freertos log http_server esp_http_server
)
