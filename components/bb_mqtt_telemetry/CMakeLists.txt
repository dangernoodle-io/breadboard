idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mqtt_telemetry/bb_mqtt_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_mqtt_client bb_core bb_telemetry
    PRIV_REQUIRES bb_log bb_init bb_json bb_nv bb_tls_creds bb_pub bb_sink_mqtt)

if(CONFIG_BB_MQTT_TELEMETRY_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_mqtt_telemetry)
endif()
