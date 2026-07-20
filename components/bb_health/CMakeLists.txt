idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health_stack.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_common.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_health/bb_health_emit.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_wire.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_wire.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_section.c"
    INCLUDE_DIRS "include"
    # bb_serialize: PUBLIC -- include/bb_health_stack_wire.h (the
    # "health.stack" wire descriptor, B1-1045 PR-2) includes bb_serialize.h.
    # bb_health_wire_priv.h (v2 /api/health wire descriptor, B1-767 PR-6)
    # stays a private header, not exposed under include/, but bb_serialize
    # is now needed publicly regardless for the health.stack descriptor.
    # bb_registry: PRIVATE -- bb_health_section.c (the composer-shaped
    # section registry, B1-1096 PR-1) stores the name -> section table in a
    # bb_registry instance, an implementation detail never exposed by
    # bb_health_section.h.
    REQUIRES bb_core bb_json bb_response bb_http_server bb_serialize
    PRIV_REQUIRES bb_http bb_board bb_wifi_http bb_mdns bb_openapi bb_log bb_ota_validator
                  bb_data bb_task_registry bb_str bb_registry
)
