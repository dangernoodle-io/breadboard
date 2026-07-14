idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_telemetry/bb_pub_telemetry_host.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_pub bb_core bb_telemetry
    PRIV_REQUIRES bb_board bb_log bb_ntp bb_json bb_system bb_str)
