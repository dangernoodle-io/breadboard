# bb_nv stays REQUIRES for bb_nv_namespaces.h's BB_MQTT_NVS_NS constant
# (public header dep, see bb_mqtt_client.h) and bb_nv_keys.h's
# BB_NV_KEY_CLIENT_ID (B1-756) -- enabled/uri/client_id/username/password/tls
# no longer round-trip through bb_nv's generic-KV API, that logic now lives
# in bb_config below.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_client/bb_mqtt_client_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_reassemble.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_tls_creds bb_tls bb_nv
    PRIV_REQUIRES bb_config mqtt mbedtls bb_log bb_wifi bb_event bb_str bb_settings)
