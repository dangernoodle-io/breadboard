idf_component_register(
    SRCS "src/ota_validator.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_nv bb_http app_update
)
