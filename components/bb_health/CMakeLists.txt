idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health_stack.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_common.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_health/bb_health_emit.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_response
    PRIV_REQUIRES bb_http bb_http_server bb_board bb_wifi bb_mdns bb_openapi bb_init bb_log bb_ota_validator
                  bb_event bb_event_routes bb_task_registry
)

if(CONFIG_BB_HEALTH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_health)
    bb_init_force_register(${COMPONENT_LIB} bb_health)
endif()
if(CONFIG_BB_HEALTH_STACK_AUTOSTART)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_health_stack)
endif()
