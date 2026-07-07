idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_client/bb_mqtt_client_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_reassemble.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_tls_creds bb_tls bb_nv
    PRIV_REQUIRES mqtt mbedtls bb_log bb_init bb_wifi bb_str)

if(CONFIG_BB_MQTT_CLIENT_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_mqtt_client)
endif()
