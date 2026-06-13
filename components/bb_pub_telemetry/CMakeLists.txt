idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_telemetry/bb_pub_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_pub bb_core bb_telemetry
    PRIV_REQUIRES bb_log bb_registry bb_json esp_timer)

if(CONFIG_BB_PUB_TELEMETRY_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub_telemetry)
endif()
