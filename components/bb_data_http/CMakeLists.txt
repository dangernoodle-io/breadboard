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
#
# bb_http (B1-1215): bb_data_http_internal.h includes it directly for
# bb_route_t/bb_route_response_t/bb_route_param_t -- the GET /api/events
# descriptor (bb_data_http_events_route()) that fixes /api/events' absence
# from /api/openapi.json. Direct-use dependency, not transitive padding on
# bb_http_server's own bb_http edge (same posture bb_diag/bb_sensor_http use
# for their own PRIV_REQUIRES bb_http).
#
# bb_ws_server (B1-1050 PR-1): the espidf backend's WS egress branch
# (espidf_send_fn's is_ws path, ws_connect_cb/ws_disconnect_cb, GET
# /ws/events) wraps bb_ws_server_broadcast_frame_async/register_endpoint/
# set_connect_cb/set_disconnect_cb directly -- same PRIV_REQUIRES posture as
# bb_http_server above (backend-internal, never in the pure core's public
# header).
idf_component_register(
    SRCS "src/bb_data_http_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_data_http/bb_data_http_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_queue bb_core
    PRIV_REQUIRES bb_log bb_registry bb_str bb_data bb_http bb_http_server bb_ws_server bb_task lwip freertos)
