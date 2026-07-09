# bb_log_event — "log" bb_event stream topic sink, carved out of bb_log
# (KB #708 / #704). Requires bb_log's base + bb_event stack; bb_log itself
# has zero dependency back on this component, breaking the former
# bb_http<->bb_log circular dependency.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event/bb_log_event.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_log_event/bb_log_event_parse.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event"
    REQUIRES bb_log bb_event bb_event_routes bb_openapi bb_json bb_task
             bb_core bb_http_server
)
