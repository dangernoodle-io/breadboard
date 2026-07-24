# bb_diag + bb_serialize: REQUIRES -- include/bb_ws_server_diag.h (the
# "websocket" bb_diag section, B1-1077 PR-3a) includes bb_diag_section.h
# (bb_diag_fill_fn/bb_diag_section_t) and bb_serialize.h (bb_serialize_desc_t)
# directly.
# bb_serialize_meta: PUBLIC -- include/bb_ws_server_diag.h includes
# bb_serialize_meta.h directly (co-located bb_serialize_desc_meta_t table,
# gated on BB_SERIALIZE_META_SHIP; B1-1059 device-shippable PR, default off
# via CONFIG_BB_OPENAPI_RUNTIME_META); PRIV_REQUIRES doesn't propagate to a
# hand-wired external consumer of this public header.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ws_server/bb_ws_server.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_ws_server_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server bb_diag bb_serialize bb_serialize_meta
    PRIV_REQUIRES bb_log esp_http_server bb_str
)
