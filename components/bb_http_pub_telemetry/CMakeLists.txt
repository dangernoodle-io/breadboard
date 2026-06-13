idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_http_pub_telemetry/bb_http_pub_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http_pub bb_core bb_telemetry
    PRIV_REQUIRES bb_log bb_registry bb_json bb_nv bb_tls_creds)

if(CONFIG_BB_HTTP_PUB_TELEMETRY_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_http_pub_telemetry)
endif()
