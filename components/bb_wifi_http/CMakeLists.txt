# bb_diag + bb_serialize: REQUIRES -- include/bb_wifi_http_diag.h (the "wifi"
# bb_diag section, B1-1077 PR-3a) includes bb_diag_section.h
# (bb_diag_fill_fn/bb_diag_section_t) and bb_serialize.h (bb_serialize_desc_t)
# directly.
#
# bb_json dropped from REQUIRES (B1-1149): the last bb_json_t consumer in this
# component, bb_wifi_emit_status (platform/host/bb_wifi_http/
# bb_wifi_http_emit.c), is deleted -- nothing else in this component includes
# bb_json.h.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_http/bb_wifi_http_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi_http/bb_wifi_http_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_http_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_http_diag.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_http_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_http_creds_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_http_scan_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_http_apply_status.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_wifi bb_diag bb_serialize
    PRIV_REQUIRES bb_http bb_http_server bb_openapi esp_wifi bb_data bb_settings
)
