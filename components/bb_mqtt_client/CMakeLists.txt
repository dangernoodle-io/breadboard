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
# bb_serialize_meta: PUBLIC -- include/bb_mqtt_client.h includes
# bb_serialize_meta.h directly (co-located bb_serialize_desc_meta_t table,
# gated on BB_SERIALIZE_META_SHIP; B1-1059 device-shippable PR, default off
# via CONFIG_BB_OPENAPI_RUNTIME_META); PRIV_REQUIRES doesn't propagate to a
# hand-wired external consumer of this public header.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_client/bb_mqtt_client_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_reassemble.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_health.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_mqtt_client_health_section.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_tls_creds bb_tls bb_serialize bb_health bb_serialize_meta
    PRIV_REQUIRES bb_config mqtt mbedtls bb_log bb_wifi bb_lifecycle bb_str bb_settings)
