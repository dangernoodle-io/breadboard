idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt/bb_mqtt_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_tls_creds
    PRIV_REQUIRES mqtt mbedtls bb_nv bb_log bb_registry bb_wifi)

if(CONFIG_BB_MQTT_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_early(${COMPONENT_LIB} bb_mqtt)
endif()
