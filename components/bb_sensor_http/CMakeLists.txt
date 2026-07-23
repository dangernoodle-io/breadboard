idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/bb_sensor_http_wire.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_sensor_http_dispatch.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sensor_http/bb_sensor_http.c"
    INCLUDE_DIRS "include" "."
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_sensor bb_data bb_serialize bb_http
)

# bb_http: PRIVATE, added (B1-1180 PR-2) -- bb_sensor_http_wire.c defines
# PRODUCER-OWNED `static const bb_route_t` describe-only routes (bb_route_t
# lives in bb_http.h, the lean route-descriptor-only component -- NOT just
# bb_http_server, which is already a public REQUIRES above but whose own
# transitive bb_http dependency the no-transitive-padding rule doesn't permit
# as the sole justification for this direct #include "bb_http.h" call) for
# GET+PATCH /api/sensors/fan, GET /api/sensors/power, and GET
# /api/sensors/thermal, wired into bb_openapi via
# bb_http_register_route_descriptor_only() (bb_http_server.h) from
# bb_sensor_http_describe_routes() -- mirrors bb_diag's exact precedent (see
# components/bb_diag/CMakeLists.txt's own bb_http comment).
