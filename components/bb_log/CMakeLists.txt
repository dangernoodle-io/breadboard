set(_log_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_level_backend.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_panic.c"
              "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_level.c")
set(_log_reqs bb_core esp_ringbuf freertos log bb_nv)
set(_log_priv_reqs bb_http bb_json bb_registry)

if(CONFIG_BB_LOG_ROUTES)
    list(APPEND _log_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_routes.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_http.c")
endif()

idf_component_register(
    SRCS ${_log_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${_log_reqs}
    PRIV_REQUIRES ${_log_priv_reqs}
)

if(CONFIG_BB_LOG_ROUTES)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_log_register_routes)
    bb_registry_force_register(${COMPONENT_LIB} bb_log_stream_register_routes)
endif()

if(CONFIG_BB_LOG_STREAM_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_early(${COMPONENT_LIB} bb_log_stream)
endif()
