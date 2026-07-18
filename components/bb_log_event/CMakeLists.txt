# bb_log_event — "log" bb_event stream topic sink, carved out of bb_log
# (KB #708 / #704). Requires bb_log's base + bb_event stack; bb_log itself
# has zero dependency back on this component, breaking the former
# bb_http<->bb_log circular dependency.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event/bb_log_event.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_event_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_log_event/bb_log_event_parse.c"
    INCLUDE_DIRS "include" "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event"
    # bb_serialize: PUBLIC -- include/bb_log_event_wire.h (the "log" wire
    # descriptor, B1-1045 PR-2) includes bb_serialize.h and is codegen-visible.
    # bb_str: PRIV -- bb_strlcpy for the last-log stash + gather (bb_log_event.c).
    REQUIRES bb_log bb_event bb_event_routes bb_openapi bb_json bb_task
             bb_core bb_http_server bb_serialize
    PRIV_REQUIRES bb_str
)
