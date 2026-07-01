set(_log_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_level_backend.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_log/bb_log_event_parse.c"
              "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_level.c")
set(_log_reqs bb_core)
set(_log_priv_reqs bb_http bb_json bb_init esp_ringbuf freertos log bb_nv lwip bb_sse_writer bb_event bb_event_routes bb_openapi)

if(CONFIG_BB_LOG_ROUTES)
    list(APPEND _log_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_http.c")
endif()

if(CONFIG_BB_LOG_EVENT_AUTO_ATTACH)
    list(APPEND _log_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_event.c")
endif()

idf_component_register(
    SRCS ${_log_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${_log_reqs}
    PRIV_REQUIRES ${_log_priv_reqs}
)

if(CONFIG_BB_LOG_ROUTES)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_log_register_routes)
    bb_init_force_register(${COMPONENT_LIB} bb_log_register_routes)
endif()

if(CONFIG_BB_LOG_STREAM_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_log_stream)
endif()

if(CONFIG_BB_LOG_EVENT_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_log_event)
endif()
