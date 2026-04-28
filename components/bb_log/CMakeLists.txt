idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_level_backend.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_level.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf freertos log bb_nv
)
