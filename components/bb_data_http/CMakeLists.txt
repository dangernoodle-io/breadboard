# bb_data_http's pure core has no FreeRTOS/httpd/bb_data/bb_ws_server
# dependency of its own -- render/generation/send are injected fn-pointer
# seams the composition root wires up. REQUIRES stays bb_queue + bb_core
# only; bb_registry is PRIV_REQUIRES (internal attach-table storage, not part
# of the public API surface).
#
# The ESP-IDF backend (B1-1033 first bench-flash de-risk, KB 1447) is a
# flat per-platform TU added to THIS component's own SRCS -- same
# backend-dispatch shape as bb_timer/bb_wifi/bb_http_server, not a separate
# platform component. It wraps bb_data (render/generation) and
# bb_http_server (async req + socket) behind the injected seams above, so
# both land in PRIV_REQUIRES -- the pure core's public header still never
# mentions either.
idf_component_register(
    SRCS "src/bb_data_http_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_data_http/bb_data_http_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_queue bb_core
    PRIV_REQUIRES bb_log bb_registry bb_str bb_data bb_http_server bb_task lwip freertos)
