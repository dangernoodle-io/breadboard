idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_telemetry/bb_telemetry.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_telemetry/bb_telemetry_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_section bb_http bb_registry bb_log bb_pub bb_nv esp_timer)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
bb_registry_force_register(${COMPONENT_LIB} bb_telemetry)
