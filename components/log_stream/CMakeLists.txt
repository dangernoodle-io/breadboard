idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/log_stream/log_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf freertos log
)
