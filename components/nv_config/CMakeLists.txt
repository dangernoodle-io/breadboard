idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/nv_config/nv_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log_stream
)
