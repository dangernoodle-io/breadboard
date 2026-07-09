idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health_stack.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_common.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_health/bb_health_emit.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_response bb_http_server
    PRIV_REQUIRES bb_http bb_board bb_wifi_http bb_mdns bb_openapi bb_log bb_ota_validator
                  bb_event bb_event_routes bb_task_registry
)
