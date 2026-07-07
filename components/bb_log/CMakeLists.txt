set(_log_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_level_backend.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_log/bb_log_event_parse.c"
              "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_level.c"
              "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_config.c")
set(_log_reqs bb_core)
set(_log_priv_reqs bb_http bb_http_server bb_json bb_init bb_kv esp_ringbuf freertos log bb_nv lwip bb_sse_writer bb_event bb_event_routes bb_openapi bb_task_registry bb_str)

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

if(CONFIG_BB_LOG_CONFIG_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_log_config)
endif()

if(CONFIG_BB_LOG_EVENT_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_log_event)
endif()

# bb_log.c is portable (guarded by #ifdef ESP_PLATFORM internally) but filed
# under platform/espidf/ for historical reasons; the host build needs it too.
# bbtool-scaffold-hint: source=platform/espidf/bb_log/bb_log.c
