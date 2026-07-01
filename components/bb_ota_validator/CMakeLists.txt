idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_validator/bb_ota_validator.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv
    PRIV_REQUIRES bb_http bb_json bb_log bb_init app_update
)

if(CONFIG_BB_OTA_VALIDATOR_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_ota_validator)
    bb_init_force_register(${COMPONENT_LIB} bb_ota_validator)
endif()
