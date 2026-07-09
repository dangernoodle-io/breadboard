idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mqtt_telemetry/bb_mqtt_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_mqtt_client bb_core bb_telemetry
    PRIV_REQUIRES bb_log bb_json bb_nv bb_tls_creds bb_pub bb_sink_mqtt)
