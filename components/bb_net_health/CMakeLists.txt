idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_net_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_espidf.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_board bb_cache bb_mqtt bb_health bb_wifi bb_log bb_json bb_event bb_event_routes bb_timer bb_pub bb_sink_http bb_openapi bb_http bb_init bb_transport_health
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_NET_HEALTH_ROUTES_AUTOREGISTER)
    bb_init_force_register(${COMPONENT_LIB} bb_net_health_routes)
endif()
if(CONFIG_BB_NET_HEALTH_AUTOSTART)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_net_health)
endif()
