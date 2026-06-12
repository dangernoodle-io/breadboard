idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mqtt_routes/bb_mqtt_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_mqtt bb_core
    PRIV_REQUIRES bb_http bb_log bb_registry bb_json bb_nv bb_tls_creds)

if(CONFIG_BB_MQTT_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_mqtt_routes)
endif()
