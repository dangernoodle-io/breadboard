# bb_nv_namespaces.h's BB_NET_HEALTH_EGRESS_ACT_NVS_NS / bb_nv_keys.h's
# BB_NET_HEALTH_EGRESS_ACT_KEY_STATE constants now live in bb_config
# (B1-958, bb_nv dissolution epic B1-708) -- bb_nv is no longer required at
# all here; the persisted egress-recovery reboot-rate-limit state
# round-trips through bb_config below.
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_net_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_espidf.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_net_health/bb_net_health_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_board bb_mqtt_client bb_health bb_wifi bb_log bb_json bb_timer bb_http bb_transport_health bb_config bb_ntp bb_system esp_system bb_str
)
