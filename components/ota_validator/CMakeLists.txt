idf_component_register(
    SRCS "src/ota_validator.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config http_server app_update
)
