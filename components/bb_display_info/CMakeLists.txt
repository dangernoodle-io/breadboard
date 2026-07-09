idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_info/bb_display_info.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_display_info_event_common.c"
    INCLUDE_DIRS "include" "."
    REQUIRES bb_display bb_info bb_nv bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_json bb_event bb_event_routes bb_openapi bb_cache bb_str)
