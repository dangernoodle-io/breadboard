idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_alert/bb_alert.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_alert/bb_alert_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_event bb_event_routes bb_log bb_openapi bb_init)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_ALERT_AUTOREGISTER)
    bb_init_force_register(${COMPONENT_LIB} bb_alert)
endif()
