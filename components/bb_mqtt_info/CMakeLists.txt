idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_info/bb_mqtt_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_mqtt bb_health bb_core
    PRIV_REQUIRES bb_log bb_json)
