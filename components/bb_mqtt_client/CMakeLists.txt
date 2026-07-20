# bb_mqtt_client owns its NVS namespace/key constants internally
# (src/bb_mqtt_client_nvs.h).
#
# bb_serialize: REQUIRES -- bb_mqtt_client.h is a public header that itself
# includes bb_serialize.h (bb_serialize_desc_t, B1-1040 per-instance
# health), so bb_serialize must be public here too (mirrors bb_tcp_client's
# CMakeLists.txt). bb_clock_now_ms64() (src/bb_mqtt_client_health.c's
# last_ok_ms source) lives in bb_core, already REQUIRES'd below.
#
# bb_health: REQUIRES -- bb_mqtt_client.h now also includes bb_health_section.h
# (bb_health_fill_args_t, bb_health_section_t; B1-1099, folding the deleted
# bb_mqtt_info's "mqtt" section producer onto the bb_health_section seam),
# so bb_health must be public here too (mirrors bb_temp's CMakeLists.txt).
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_client/bb_mqtt_client_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_reassemble.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_health.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_health_section.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_tls_creds bb_tls bb_serialize bb_health
    PRIV_REQUIRES bb_config mqtt mbedtls bb_log bb_wifi bb_lifecycle bb_str bb_settings)
