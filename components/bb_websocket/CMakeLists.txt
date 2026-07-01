set(srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_websocket/bb_websocket.c")

# NOTE: ESP-IDF resolves REQUIRES/PRIV_REQUIRES in an early pass before
# Kconfig values are available, so this list must stay unconditional (a
# CONFIG_ gate here silently drops the dependency). Only SRCS and the
# bb_init_force_register call below are gated -- matches bb_temp,
# bb_partition, bb_ota_boot, bb_power_routes, bb_sink_http_telemetry.
if(CONFIG_BB_WEBSOCKET_DIAG_AUTOREGISTER)
    list(APPEND srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_websocket/bb_websocket_diag.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http
    PRIV_REQUIRES bb_log bb_init esp_http_server
)

if(CONFIG_BB_WEBSOCKET_DIAG_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_websocket_diag)
endif()
