idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt/bb_mqtt_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_tls_creds bb_tls
    PRIV_REQUIRES mqtt mbedtls bb_nv bb_log bb_init bb_wifi)

if(CONFIG_BB_MQTT_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_mqtt)
endif()
