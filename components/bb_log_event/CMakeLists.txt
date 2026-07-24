# bb_log_event — "log" bb_data key sink, carved out of bb_log (KB #708 /
# #704). Requires bb_log's base; bb_log itself has zero dependency back on
# this component, breaking the former bb_http<->bb_log circular dependency.
# bb_serialize_meta: PRIV_REQUIRES -- co-located bb_serialize_desc_meta_t table(s) in this component's own .c files, gated on BB_SERIALIZE_META_SHIP (B1-1059 device-shippable PR; default off via CONFIG_BB_OPENAPI_RUNTIME_META).
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event/bb_log_event.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_event_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_event_line_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_log_event/bb_log_event_parse.c"
    INCLUDE_DIRS "include" "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event"
    # bb_serialize: PUBLIC -- include/bb_log_event_wire.h (the "log" wire
    # descriptor, B1-1045 PR-2) includes bb_serialize.h and is codegen-visible.
    # bb_str: PRIV -- bb_strlcpy for the last-log stash + gather (bb_log_event.c).
    # bb_data: PRIV -- bb_data_touch("log") (B1-1045 cutover).
    # bb_serialize_json: PRIV -- bb_serialize_json_render() for the internal
    # per-log-line JSON build (bb_log_event.c s_forwarder_task); replaces the
    # former bb_json (cJSON) dependency (no-heap migration).
    REQUIRES bb_log bb_openapi bb_task
             bb_core bb_http_server bb_serialize
    PRIV_REQUIRES bb_str bb_data bb_serialize_json bb_serialize_meta
)
