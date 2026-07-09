# bb_log_http — GET/POST /api/log/level routes sink, carved out of bb_log
# (KB #708 / #704). Requires bb_log's base + bb_http_server; bb_log itself
# has zero dependency back on this component, breaking the former
# bb_http_server<->bb_log circular dependency.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_http/bb_log_http.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_http"
    REQUIRES bb_http_server
    PRIV_REQUIRES bb_log
)
