idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_telemetry/bb_telemetry.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_telemetry/bb_telemetry_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_response bb_http bb_http_server bb_init bb_log bb_pub bb_nv)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
bb_init_force_register(${COMPONENT_LIB} bb_telemetry)

# bb_telemetry_routes.c is portable (no ESP-IDF-specific includes) but filed
# under platform/espidf/ only; the host build needs it alongside the existing
# platform/host/bb_telemetry/bb_telemetry.c.
# bbtool-scaffold-hint: source=platform/espidf/bb_telemetry/bb_telemetry_routes.c
