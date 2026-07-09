idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_health/bb_pub_health.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub bb_health bb_mqtt_client
    PRIV_REQUIRES bb_json bb_log bb_net_health)
