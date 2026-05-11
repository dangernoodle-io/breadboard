set(_diag_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_panic.c")

if(CONFIG_BB_DIAG_ROUTES)
    list(APPEND _diag_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_routes.c")
endif()

idf_component_register(
    SRCS ${_diag_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_registry bb_http bb_json bb_info bb_nv espcoredump
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

if(CONFIG_BB_DIAG_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_early(${COMPONENT_LIB} bb_diag_panic)
    if(CONFIG_BB_DIAG_ROUTES)
        bb_registry_force_register(${COMPONENT_LIB} bb_diag_routes)
    endif()
endif()
