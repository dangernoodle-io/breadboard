set(_diag_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_panic.c")

if(CONFIG_BB_DIAG_ROUTES)
    list(APPEND _diag_srcs
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_diag/bb_diag_routes.c")
endif()

idf_component_register(
    SRCS ${_diag_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_registry bb_http bb_json bb_info espcoredump
)

if(CONFIG_BB_DIAG_PANIC_COREDUMP)
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
