# bb_serialize: PUBLIC -- include/bb_ota_check_wire.h (the "update.available"
# wire descriptor, B1-1045 PR-2) includes bb_serialize.h.
# bb_json: DROPPED (B1-1053 PR3) -- the last bb_json use in this component
# (bb_ota_check_serialize(), src/bb_ota_check_common.c) was deleted; the
# "update.available" REST GET path now renders via bb_data instead (see
# bb_ota_check_wire.c / bb_ota_check_emit_status_json()). bb_http_server
# (still PRIV_REQUIRES below) does not need bb_json either -- its streaming
# JSON obj API is hand-rolled, cJSON-free.
# bb_serialize_meta: PUBLIC -- include/bb_ota_check_wire.h includes
# bb_serialize_meta.h directly (co-located bb_serialize_desc_meta_t table,
# gated on BB_SERIALIZE_META_SHIP; B1-1059 device-shippable PR, default off
# via CONFIG_BB_OPENAPI_RUNTIME_META); PRIV_REQUIRES doesn't propagate to a
# hand-wired external consumer of this public header.
idf_component_register(
    SRCS "src/bb_ota_check_common.c"
         "src/bb_ota_check_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_check/bb_ota_check_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_release_manifest bb_serialize bb_serialize_meta
    PRIV_REQUIRES bb_http_client bb_data bb_http bb_http_server bb_log bb_mdns bb_settings bb_openapi bb_system esp_http_server bb_timer esp_hw_support bb_cache bb_task bb_str)
