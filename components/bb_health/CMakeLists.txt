idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_health/bb_health_stack.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_common.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_health/bb_health_emit.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_wire.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_stack_wire.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_section.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_health/bb_health_schema.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_health_compose.c"
    INCLUDE_DIRS "include"
    # bb_serialize: PUBLIC -- include/bb_health_stack_wire.h (the
    # "health.stack" wire descriptor, B1-1045 PR-2) includes bb_serialize.h.
    # bb_health_wire_priv.h (v2 /api/health wire descriptor, B1-767 PR-6)
    # stays a private header, not exposed under include/, but bb_serialize
    # is now needed publicly regardless for the health.stack descriptor.
    # bb_response and bb_json DROPPED (B1-1100): the live /api/health
    # handler no longer assembles via bb_response/bb_json_t -- it composes
    # from bb_health_section + bb_serialize via bb_health_compose_and_stream().
    # bb_registry: PRIVATE -- bb_health_section.c (the composer-shaped
    # section registry, B1-1096 PR-1) stores the name -> section table in a
    # bb_registry instance, an implementation detail never exposed by
    # bb_health_section.h.
    # bb_wifi: PRIVATE -- the ESP-IDF handler (bb_health.c) gathers the
    # root "network" slice directly via bb_wifi_get_info()/bb_wifi_info_t
    # (B1-1100), a direct dependency (bb_wifi_http's public header merely
    # transitively includes bb_wifi.h, which does not satisfy REQUIRES'
    # direct-deps-only rule for this symbol usage).
    REQUIRES bb_core bb_http_server bb_serialize
    PRIV_REQUIRES bb_http bb_wifi bb_wifi_http bb_mdns bb_openapi bb_log bb_ota_validator
                  bb_data bb_task_registry bb_str bb_registry
)
