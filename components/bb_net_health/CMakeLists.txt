idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_net_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_cache bb_mqtt bb_health bb_wifi bb_log bb_json bb_event bb_event_routes bb_timer bb_pub bb_sink_http bb_openapi
)
