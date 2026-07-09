set(_log_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log.c"
              "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log/bb_log_level_backend.c"
              "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_level.c"
              "${CMAKE_CURRENT_LIST_DIR}/src/bb_log_config.c")
set(_log_reqs bb_core)
# Core deps: genuinely referenced by the always-built sources (bb_log.c,
# bb_log_level_backend.c, bb_log_level.c, bb_log_config.c).
# bb_task creates the writer task unconditionally in bb_log_stream_init()
# (Option C, KB #731) -- bb_task is a floor-safe primitive (REQUIRES bb_core;
# PRIV_REQUIRES bb_registry freertos bb_str, no bb_log dep) that upserts a
# passive base registry entry as a side effect of xTaskCreate*, so this no
# longer needs bb_task_registry (which drags in bb_init).
#
# KB #708: this base component intentionally has ZERO bb_http/bb_event/
# bb_event_routes/bb_openapi/bb_json deps -- those live in the bb_log_http /
# bb_log_event sink components (components/bb_log_http, components/bb_log_event),
# which REQUIRE this component. Previously bb_http<->bb_log formed a circular
# component dependency once CONFIG_BB_LOG_ROUTES/CONFIG_BB_LOG_EVENT_AUTO_ATTACH
# pulled bb_http/bb_event into bb_log's own PRIV_REQUIRES; splitting the sinks
# into their own components breaks the cycle at the source.
set(_log_priv_reqs bb_str esp_ringbuf freertos log bb_task)

if(CONFIG_BB_LOG_UDP_SINK)
    # The UDP mirror sink is deeply entangled in s_log_vprintf (bb_log.c) --
    # left in base rather than extracted into its own component. Only adds a
    # conditional lwip dep, gated off by default (no floor/discovery impact).
    list(APPEND _log_priv_reqs lwip)
endif()

idf_component_register(
    SRCS ${_log_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${_log_reqs}
    PRIV_REQUIRES ${_log_priv_reqs}
)

# bb_log.c is portable (guarded by #ifdef ESP_PLATFORM internally) but filed
# under platform/espidf/ for historical reasons; the host build needs it too.
# bbtool-scaffold-hint: source=platform/espidf/bb_log/bb_log.c
