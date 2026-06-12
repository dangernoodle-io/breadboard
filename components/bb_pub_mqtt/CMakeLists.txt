idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_mqtt/bb_pub_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub bb_mqtt)
