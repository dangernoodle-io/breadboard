idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_sub_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_sub bb_mqtt_client bb_nv bb_log bb_init bb_transport_health)

if(CONFIG_BB_SUB_MQTT_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_sub_mqtt)
endif()
