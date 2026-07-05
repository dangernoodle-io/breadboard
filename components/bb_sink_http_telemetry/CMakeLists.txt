idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_http_telemetry/bb_sink_http_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_sink_http bb_core bb_telemetry
    PRIV_REQUIRES bb_log bb_init bb_json bb_nv bb_tls_creds bb_pub bb_sink_http bb_str)

if(CONFIG_BB_SINK_HTTP_TELEMETRY_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_sink_http_telemetry)
endif()
