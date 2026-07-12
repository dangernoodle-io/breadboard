idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_info/bb_info.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_info_build.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_info_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv bb_json bb_response bb_http_server
    # bb_ota_pull provides bb_ota_pull_heap_ready() for the ota_ready field. The
    # *emission* is gated in bb_info.c (BB_INFO_EMIT_OTA_READY) so --gc-sections
    # drops bb_ota_pull on boards with no runtime OTA-TLS path. REQUIRES itself
    # can't be CONFIG-conditional — ESP-IDF expands deps before config is loaded.
    # bb_cache/bb_event/bb_event_routes: PRIV_REQUIRES because they are not
    # exposed in any public header under components/bb_info/include/.
    # bb_serialize: PRIV_REQUIRES -- bb_info_wire_priv.h (v2 wire descriptor,
    # B1-767 PR-6) is a private header, not exposed under include/.
    PRIV_REQUIRES bb_http bb_board bb_wifi bb_openapi bb_log bb_ntp bb_mdns bb_ota_pull bb_cache bb_event bb_event_routes bb_system bb_str bb_serialize
)
