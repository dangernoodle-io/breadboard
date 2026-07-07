idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_info/bb_mqtt_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_mqtt_client bb_health bb_core
    PRIV_REQUIRES bb_log bb_json bb_init)

if(CONFIG_BB_MQTT_INFO_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_mqtt_info)
endif()
