idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_net_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_espidf.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_board bb_cache bb_mqtt_client bb_health bb_wifi bb_log bb_json bb_event bb_event_routes bb_timer bb_openapi bb_http bb_transport_health bb_nv bb_ntp bb_system esp_system bb_str
)
