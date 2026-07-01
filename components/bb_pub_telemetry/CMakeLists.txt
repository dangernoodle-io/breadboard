idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_telemetry/bb_pub_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_pub bb_core bb_telemetry
    PRIV_REQUIRES bb_board bb_info bb_log bb_ntp bb_init bb_json bb_system)

if(CONFIG_BB_PUB_TELEMETRY_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_pub_telemetry)
endif()
