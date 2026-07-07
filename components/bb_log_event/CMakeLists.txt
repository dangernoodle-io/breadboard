# bb_log_event — "log" bb_event stream topic sink, carved out of bb_log
# (KB #708 / #704). Requires bb_log's base + bb_event stack; bb_log itself
# has zero dependency back on this component, breaking the former
# bb_http<->bb_log circular dependency.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_event/bb_log_event.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_log_event/bb_log_event_parse.c"
    REQUIRES bb_log bb_event bb_event_routes bb_openapi bb_json bb_task_registry
             bb_init bb_core
    PRIV_REQUIRES bb_http_server
)

if(CONFIG_BB_LOG_EVENT_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_log_event)
endif()
