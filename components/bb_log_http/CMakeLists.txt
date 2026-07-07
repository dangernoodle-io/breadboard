# bb_log_http — GET/POST /api/log/level routes sink, carved out of bb_log
# (KB #708 / #704). Requires bb_log's base + bb_http_server; bb_log itself
# has zero dependency back on this component, breaking the former
# bb_http_server<->bb_log circular dependency.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_http/bb_log_http.c"
    PRIV_REQUIRES bb_http_server bb_log bb_init
)

if(CONFIG_BB_LOG_ROUTES)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_log_register_routes)
    bb_init_force_register(${COMPONENT_LIB} bb_log_register_routes)
endif()
