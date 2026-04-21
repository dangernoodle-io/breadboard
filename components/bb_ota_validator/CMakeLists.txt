idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_validator/bb_ota_validator.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_nv bb_http bb_log app_update
)
