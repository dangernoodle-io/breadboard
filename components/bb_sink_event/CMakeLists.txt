idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_event/bb_sink_event.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub
    PRIV_REQUIRES bb_event bb_event_routes bb_json bb_log bb_str)
