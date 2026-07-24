# bb_serialize_meta: PUBLIC -- include/bb_temp.h includes bb_serialize_meta.h
# directly (co-located bb_serialize_desc_meta_t table, gated on
# BB_SERIALIZE_META_SHIP; B1-1059 device-shippable PR, default off via
# CONFIG_BB_OPENAPI_RUNTIME_META); PRIV_REQUIRES doesn't propagate to a
# hand-wired external consumer of this public header.
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_temp/bb_temp.c"
    INCLUDE_DIRS "include"
    # bb_serialize: PUBLIC -- include/bb_temp.h now declares bb_temp_health_desc
    # (a bb_serialize_desc_t) and bb_temp_health_fill() directly (B1-1098).
    # bb_json/bb_http_server dropped: the legacy bb_json_t emit path and the
    # http_server handle parameter are both gone -- registering a
    # /api/health section needs neither.
    REQUIRES bb_health bb_serialize bb_core bb_serialize_meta
    PRIV_REQUIRES bb_log bb_system)
