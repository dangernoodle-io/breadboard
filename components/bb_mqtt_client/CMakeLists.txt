# bb_mqtt_client owns its NVS namespace/key constants internally
# (src/bb_mqtt_client_nvs.h).
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_client/bb_mqtt_client_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_reassemble.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_tls_creds bb_tls
    PRIV_REQUIRES bb_config mqtt mbedtls bb_log bb_wifi bb_event bb_str bb_settings)
