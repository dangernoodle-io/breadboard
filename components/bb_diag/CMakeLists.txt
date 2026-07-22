set(_diag_srcs
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_reset_decision.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_scrub.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_boot_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_section.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_meminfo.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_storage_nvs.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_diag_storage_partitions.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_panic.c"
)

# bb_diag_routes.c (CONFIG_BB_DIAG_ROUTES) and bb_diag_section_dispatch.c
# (CONFIG_BB_DIAG_SECTIONS) relocated wholesale to the new bb_diag_http
# component (B1-1153, KB 1477 -- an SSOT component owns NO routes; its HTTP
# surface lives in a sibling component, bb_wifi_http's already-shipped
# shape). bb_diag is bb_http_server-free after this split -- see
# components/bb_diag_http/CMakeLists.txt for the two Kconfig-gated route
# surfaces that moved.

# bb_nv_namespaces.h's BB_REBOOT_NVS_NS / bb_nv_keys.h's
# BB_REBOOT_KEY_LAST/BB_REBOOT_KEY_HISTORY constants now live in bb_config
# (B1-958, bb_nv dissolution epic B1-708) -- bb_nv is no longer required at
# all here; the reboot-reason SSOT's persisted record/history round-trips
# through bb_config below.
# The DELETE /api/nvs route (and the private-include boundary violation it
# required to reach bb_nv's implementation dir) moved out to bb_storage_http
# (B1-757) -- since dissolved into bb_diag_http (B1-1154) -- bb_diag no
# longer wires or depends on it.
# bb_storage is now an explicit PRIV_REQUIRES: bb_diag_panic.c calls
# bb_storage_txn_begin/set/commit directly (B1-756's last bb_nv migration,
# PR-B) -- previously only transitively available via bb_config.h, which the
# no-transitive-padding rule doesn't permit as the sole justification for a
# direct-call dependency.
# bb_serialize: PUBLIC -- include/bb_diag_boot_wire.h (the "diag.boot" wire
# descriptor, B1-1045 PR-2) includes bb_serialize.h.
# bb_http_server: DROPPED (B1-1153) -- bb_diag_boot_wire.c's sole use of it
# (bb_diag_boot_render_envelope()) moved to bb_diag_http along with every
# other route/handler; bb_diag itself never calls into bb_http_server again.
# bb_http/bb_ntp/bb_ota_validator/bb_system/bb_task_registry/bb_task/
# bb_openapi/lwip: DROPPED (B1-1153) -- every remaining call site
# (bb_diag_routes.c's load_reboot_record/build_boot_snap/panic_get_handler/
# heap_get_handler/tasks_get_handler/sockets_get_handler and
# bb_diag_section_dispatch.c) moved to bb_diag_http verbatim; nothing left in
# bb_diag itself calls into any of them.
# bb_json: DROPPED (B1-1053 PR1) -- the last bb_json use in this component
# (bb_diag_boot_serialize(), bb_diag_event_common.c) was deleted; the
# "diag.boot" REST GET path now renders via bb_data instead (see
# bb_diag_boot_wire.c).
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
idf_component_register(
    SRCS ${_diag_srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES bb_core bb_serialize
    PRIV_REQUIRES bb_log bb_cache bb_config bb_storage bb_data espcoredump nvs_flash esp_partition esp_app_format bb_str bb_registry bb_partition bb_settings bb_meminfo
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
