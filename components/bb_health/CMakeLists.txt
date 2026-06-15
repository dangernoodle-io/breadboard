idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health_stack.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_common.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_health/bb_health_emit.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_http bb_board bb_wifi bb_mdns bb_registry bb_log bb_ota_validator
                  bb_event bb_event_routes bb_timer
)

if(CONFIG_BB_HEALTH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_health)
    bb_registry_force_register(${COMPONENT_LIB} bb_health)
endif()
