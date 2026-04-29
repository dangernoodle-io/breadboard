idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_validator/bb_ota_validator.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv app_update
    PRIV_REQUIRES bb_http bb_log bb_registry
)

if(CONFIG_BB_OTA_VALIDATOR_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_ota_validator)
endif()
