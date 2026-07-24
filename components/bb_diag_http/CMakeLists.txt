# bb_diag_http — the aggregate HTTP surface for bb_diag (B1-1153, KB 1477):
# an SSOT component owns NO routes; its HTTP surface lives here instead,
# the same split bb_wifi_http already shipped for bb_wifi. Unlike
# bb_wifi_http (which owns its own private headers), this component
# currently reaches bb_diag's shared internal headers (bb_diag_event_priv.h,
# bb_diag_section_priv.h) via bb_diag's pre-existing public-root include dir
# (INCLUDE_DIRS "include" "." on bb_diag's own CMakeLists.txt, predating
# this PR) rather than PRIV_INCLUDE_DIRS -- a known looseness, tracked
# separately as its own backlog item rather than fixed here (that fix forces
# an API-surface decision -- do those shared _priv.h headers become public?
# -- that's scope creep for a mechanical-move refactor). Two independent
# Kconfig-gated route surfaces, relocated VERBATIM from bb_diag (no
# behavior change):
#   - CONFIG_BB_DIAG_ROUTES: legacy exact routes.
#   - CONFIG_BB_DIAG_SECTIONS: the generic /api/diag/* wildcard dispatcher.
# bb_diag_http_boot_wire.c (bb_diag_boot_render_envelope()) always builds
# regardless of either Kconfig -- same as its prior home
# (components/bb_diag/bb_diag_boot_wire.c), host-testable directly.
# bb_storage_http_delete_wire.c + bb_storage_http_routes.c (B1-1154, KB
# 1477): a THIRD independent Kconfig-gated route surface, dissolved in here
# from the former bb_storage_http component -- DELETE /api/diag/storage
# always builds (same posture as bb_diag_http_boot_wire.c above); POST
# /api/diag/factory-reset is internally gated by the routes file's own
# CONFIG_BB_STORAGE_HTTP_FACTORY_RESET #if (unrelated to BB_DIAG_ROUTES/
# BB_DIAG_SECTIONS above -- that Kconfig symbol KEEPS its bb_storage_http-era
# name; renaming it would break any sdkconfig.defaults already setting it).
# bb_log_http.c (B1-1155, KB 1477): a FOURTH independent Kconfig-gated route
# surface, dissolved in here from the former bb_log_http component --
# GET/POST /api/log/level always builds (this file's own internal
# CONFIG_BB_LOG_ROUTES #if gates registration, same posture as this
# component's other always-built sources above); CONFIG_BB_LOG_ROUTES KEEPS
# its bb_log_http-era name unchanged.
# bb_diag_heap_check_wire.c (B1-1054 diag conversion): the trivial
# proof-of-pattern bb_serialize migration for GET /api/diag/heap-check --
# same always-built posture as bb_diag_http_boot_wire.c/
# bb_storage_http_delete_wire.c above (used by bb_diag_http_routes.c, which
# is itself gated on CONFIG_BB_DIAG_ROUTES below, but the wire file itself
# stays unconditional/host-testable).
# bb_diag_panic_get_wire.c (B1-1188 diag conversion): same always-built,
# host-testable posture -- GET /api/diag/panic's bb_serialize migration
# (single unconditional descriptor, runtime present-predicate on
# coredump_avail, no Kconfig fork -- see bb_diag_panic_get_wire_priv.h).
# bb_diag_sockets_get_wire.c (B1-1190 diag conversion): same always-built,
# host-testable posture -- GET /api/diag/sockets' bb_serialize migration
# (unconditional descriptor, BB_TYPE_ARR-of-BB_TYPE_OBJ "pcbs" +
# BB_TYPE_OBJ "by_state", row cap tracks CONFIG_LWIP_MAX_SOCKETS -- see
# bb_diag_sockets_get_wire_priv.h). The wire file itself carries no LWIP
# symbols (portable); only sockets_get_handler in bb_diag_http_routes.c
# below touches LWIP directly.
# bb_diag_tasks_get_wire.c (B1-1191 diag conversion): same always-built,
# host-testable posture -- GET /api/diag/tasks' bb_serialize migration
# (unconditional row descriptor + nested "registry" BB_TYPE_OBJ, "tasks"
# streamed via BB_ARR_STREAM/bb_serialize_arr_stream_from_buf -- no fixed
# row-count cap, single-pass since uxTaskGetNumberOfTasks() gives an exact
# n up front. 4 precomputed present-flags gate 8 per-row fields; the
# genuinely-Kconfig-gated core/runtime TaskStatus_t members are resolved to
# plain (bool, int64_t) pairs in tasks_get_handler's own ESP-IDF-only
# gather -- see bb_diag_tasks_get_wire_priv.h). The wire file itself
# carries no FreeRTOS/ESP-IDF symbols (portable); only tasks_get_handler in
# bb_diag_http_routes.c below touches FreeRTOS/bb_task_registry directly.
set(_diag_http_srcs
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_http_boot_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_storage_http_delete_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_heap_check_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_panic_get_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_sockets_get_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_tasks_get_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag_http/bb_storage_http_routes.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag_http/bb_log_http.c"
)

if(CONFIG_BB_DIAG_ROUTES)
    list(APPEND _diag_http_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag_http/bb_diag_http_routes.c")
endif()

# Independent Kconfig from BB_DIAG_ROUTES (see Kconfig) -- the portable
# registry (bb_diag_section.c, stays in bb_diag) always builds; only this
# ESP-IDF wildcard-route dispatcher is gated.
if(CONFIG_BB_DIAG_SECTIONS)
    list(APPEND _diag_http_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag_http/bb_diag_http_section_dispatch.c")
endif()

# bb_diag: PUBLIC -- bb_diag_http.h includes bb_diag.h (bb_err_t, and the
# bb_diag_register_section() family every dispatched section resolves
# against); direction matters -- bb_diag_http REQUIRES bb_diag, never the
# reverse (bb_diag stays a pure SSOT with zero knowledge of its HTTP
# surface), same shape as bb_wifi_http -> bb_wifi.
# bb_http_server: PUBLIC -- bb_diag_http.h includes it directly for
# bb_http_handle_t/bb_http_request_t (every prototype here takes one).
# bb_serialize: direct use in bb_diag_http_section_dispatch.c
# (bb_serialize_query_t, bb_serialize_arr_stream_from_buf, etc) -- not solely
# riding on bb_diag's own public REQUIRES chain.
# bb_storage: PUBLIC -- bb_storage_http.h includes bb_storage.h directly
# (B1-1154, bb_storage_http dissolved into bb_diag_http, KB 1477) -- the
# DELETE /api/diag/storage route sits on the bb_storage FACADE only, never a
# concrete backend, so any registered backend (nvs today, ram/rtc/a future
# sdcard) works through this same route with no new route component per
# backend.
# bb_http/bb_cache/bb_config/bb_ntp/bb_openapi/bb_system/bb_ota_validator/
# bb_data/bb_task_registry/bb_task/espcoredump/lwip/esp_partition/bb_str:
# relocated wholesale from bb_diag's own PRIV_REQUIRES (B1-1153) -- every
# call site (load_reboot_record/build_boot_snap/panic_get_handler/
# heap_get_handler/tasks_get_handler/sockets_get_handler,
# bb_diag_http_section_dispatch.c) moved here verbatim alongside the files
# that use them.
# bb_settings/bb_timer: relocated from bb_storage_http's own PRIV_REQUIRES
# (B1-1154) -- bb_settings for the wipe_wifi guard's
# bb_settings_ns_is_wifi_creds()/bb_settings_wifi_rtc_mirror_clear() calls,
# bb_timer for the factory-reset route's deferred-reboot oneshot timer.
# bb_log_http.c (B1-1155): adds ZERO new dep edges -- bb_http_server was
# already a PUBLIC REQUIRES above (bb_log_http.c's own #include) and bb_log
# was already a PRIV_REQUIRES above (the same edge bb_log_http's own
# CMakeLists.txt PRIV_REQUIREd); bb_log itself gains no edge back onto
# bb_diag_http/bb_http_server, preserving the cycle-break this component
# existed to provide.
# bb_serialize_meta: PUBLIC -- include/bb_storage_http.h includes
# bb_serialize_meta.h directly (co-located bb_serialize_desc_meta_t table,
# gated on BB_SERIALIZE_META_SHIP; B1-1059 device-shippable PR, default off
# via CONFIG_BB_OPENAPI_RUNTIME_META); PRIV_REQUIRES doesn't propagate to a
# hand-wired external consumer of this public header.
idf_component_register(
    SRCS ${_diag_http_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_diag bb_http_server bb_serialize bb_storage bb_serialize_meta
    PRIV_REQUIRES bb_log bb_http bb_cache bb_config bb_ntp bb_openapi bb_system bb_ota_validator bb_data bb_task_registry bb_task espcoredump lwip esp_partition bb_str bb_settings bb_timer
)

if(CONFIG_BB_DIAG_PANIC_COREDUMP)
    # Same rationale as bb_diag's own CMakeLists.txt: Kconfig can't
    # `select`/`imply` into ESP-IDF's coredump choice groups, so a
    # silently-misconfigured consumer would build clean and capture nothing
    # until an actual panic exposes the gap. bb_diag_http_routes.c's own
    # coredump_get_handler() (GET /api/diag/coredump) includes esp_core_dump.h
    # directly under this same Kconfig, so this component's own target needs
    # the same espcoredump port-header include-dir treatment bb_diag's
    # CMakeLists.txt applies for bb_diag_panic.c.
    set(_bb_diag_http_missing "")
    if(NOT CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
        list(APPEND _bb_diag_http_missing "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y")
    endif()
    if(NOT CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
        list(APPEND _bb_diag_http_missing "CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y")
    endif()
    if(NOT CONFIG_ESP_COREDUMP_CHECKSUM_CRC32)
        list(APPEND _bb_diag_http_missing "CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y")
    endif()
    if(_bb_diag_http_missing)
        string(REPLACE ";" "\n  " _bb_diag_http_missing_fmt "${_bb_diag_http_missing}")
        message(FATAL_ERROR
            "CONFIG_BB_DIAG_PANIC_COREDUMP=y requires the following sdkconfig keys "
            "(set in sdkconfig.defaults or sdkconfig/<env>):\n  ${_bb_diag_http_missing_fmt}\n"
            "Also requires a 'coredump' entry in partitions.csv "
            "(see examples/smoke/partitions.*.csv).")
    endif()

    idf_component_get_property(espcoredump_dir espcoredump COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE ${espcoredump_dir}/include)
    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        target_include_directories(${COMPONENT_LIB} PRIVATE ${espcoredump_dir}/include/port/xtensa)
    elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
        target_include_directories(${COMPONENT_LIB} PRIVATE ${espcoredump_dir}/include/port/riscv)
    endif()
endif()

# bb_storage_http_routes.c is portable (no ESP-IDF-specific includes) but
# filed under platform/espidf/ (mirrors its prior bb_storage_http-owned
# location, which itself mirrored the old bb_nv_delete_routes.c location it
# replaced) -- the host scaffold's directory-convention source walk
# (scripts/bbtool/boards.py's derive_component) only looks under
# platform/host/<name>/ for the "native" board, so this hint pulls the
# portable route source into host test builds too (same pattern as bb_nv's
# three source hints, carried over from bb_storage_http's own CMakeLists.txt,
# B1-1154).
# bbtool-scaffold-hint: source=platform/espidf/bb_diag_http/bb_storage_http_routes.c
