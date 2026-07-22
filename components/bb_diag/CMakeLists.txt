set(_diag_srcs
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_reset_decision.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_scrub.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_boot_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_section.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_meminfo.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_storage_nvs.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_storage_partitions.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_cache_route_status.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_panic.c"
)

if(CONFIG_BB_DIAG_ROUTES)
    list(APPEND _diag_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_routes.c")
endif()

# Generic /api/diag/* section registry (B1-diag-dissolution PR3) -- a NEW,
# independent Kconfig from BB_DIAG_ROUTES (see Kconfig): the portable
# registry above (bb_diag_section.c) always builds; only the ESP-IDF
# wildcard-route dispatcher is gated.
if(CONFIG_BB_DIAG_SECTIONS)
    list(APPEND _diag_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_section_dispatch.c")
endif()

# bb_nv_namespaces.h's BB_REBOOT_NVS_NS / bb_nv_keys.h's
# BB_REBOOT_KEY_LAST/BB_REBOOT_KEY_HISTORY constants now live in bb_config
# (B1-958, bb_nv dissolution epic B1-708) -- bb_nv is no longer required at
# all here; the reboot-reason SSOT's persisted record/history round-trips
# through bb_config below.
# The DELETE /api/nvs route (and the private-include boundary violation it
# required to reach bb_nv's implementation dir) moved out to bb_storage_http
# (B1-757) -- bb_diag no longer wires or depends on it.
# bb_storage is now an explicit PRIV_REQUIRES: bb_diag_panic.c calls
# bb_storage_txn_begin/set/commit directly (B1-756's last bb_nv migration,
# PR-B) -- previously only transitively available via bb_config.h, which the
# no-transitive-padding rule doesn't permit as the sole justification for a
# direct-call dependency.
# bb_serialize: PUBLIC -- include/bb_diag_boot_wire.h (the "diag.boot" wire
# descriptor, B1-1045 PR-2) includes bb_serialize.h.
# bb_json: DROPPED (B1-1053 PR1) -- the last bb_json use in this component
# (bb_diag_boot_serialize(), bb_diag_event_common.c) was deleted; the
# "diag.boot" REST GET path now renders via bb_data instead (see
# bb_diag_boot_wire.c). bb_http_server (still REQUIRES above) does not need
# bb_json either -- its streaming JSON obj API is hand-rolled, cJSON-free.
# bb_registry: PRIVATE -- bb_diag_section.c (B1-diag-dissolution PR3) stores
# the name -> section table in a bb_registry instance, an implementation
# detail never exposed by bb_diag_section.h.
# bb_partition + bb_settings: PRIVATE (B1-767 PR9) -- bb_diag_storage_nvs.c/
# bb_diag_storage_partitions.c call bb_settings_nv_overlay_entries() and
# bb_partition_list()/bb_partition_row_wire_from_info() directly; bb_storage
# was already required above (bb_diag_panic.c). No bb_data dependency.
# bb_meminfo: PRIVATE (heap reconciliation, B1-diag-dissolution) --
# bb_diag_meminfo.c calls bb_meminfo_heap_snap_fill() directly; the "meminfo"
# section is a thin bb_diag_fill_fn adapter over bb_meminfo's own SSOT
# snapshot, never exposed by bb_diag_meminfo.h itself.
# bb_diag_cache_route_status.c (B1-1121): folded in from the deleted
# bb_cache_routes component -- pure bb_err_t -> HTTP status mapper backing
# GET /api/diag/cache (bb_diag_routes.c, gated CONFIG_BB_DIAG_ROUTES). Always
# built unconditionally (mirrors bb_diag_reset_decision.c/bb_diag_scrub.c
# above) so the host test suite can exercise it regardless of that gate;
# bb_cache was already a PRIV_REQUIRES here (bb_diag_boot_wire.c/
# bb_diag_routes.c), so no new dependency.
idf_component_register(
    SRCS ${_diag_srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES bb_core bb_http_server bb_serialize
    PRIV_REQUIRES bb_log bb_http bb_cache bb_config bb_storage bb_ntp bb_openapi bb_system bb_ota_validator bb_data bb_task_registry bb_task espcoredump lwip nvs_flash esp_partition esp_app_format bb_str bb_registry bb_partition bb_settings bb_meminfo
)

if(CONFIG_BB_DIAG_PANIC_COREDUMP)
    # Kconfig can't `select`/`imply` into ESP-IDF's coredump choice groups, so
    # silently-misconfigured consumers would build clean and capture nothing
    # until an actual panic exposes the gap. Fail-fast at configure time
    # instead, naming each missing key so the consumer can paste-and-fix.
    set(_bb_diag_missing "")
    if(NOT CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
        list(APPEND _bb_diag_missing "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y")
    endif()
    if(NOT CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
        list(APPEND _bb_diag_missing "CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y")
    endif()
    if(NOT CONFIG_ESP_COREDUMP_CHECKSUM_CRC32)
        list(APPEND _bb_diag_missing "CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y")
    endif()
    if(_bb_diag_missing)
        string(REPLACE ";" "\n  " _bb_diag_missing_fmt "${_bb_diag_missing}")
        message(FATAL_ERROR
            "CONFIG_BB_DIAG_PANIC_COREDUMP=y requires the following sdkconfig keys "
            "(set in sdkconfig.defaults or sdkconfig/<env>):\n  ${_bb_diag_missing_fmt}\n"
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

