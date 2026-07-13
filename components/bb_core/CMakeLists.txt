idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_core/bb_mem.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_core/bb_clock.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_core/bb_lock.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_core/bb_lock_cond.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_core/bb_claim.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_core/bb_lock_stats.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_reboot_reason.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_lock_cond_waiterlist.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_core" "${CMAKE_CURRENT_LIST_DIR}/src"
    PRIV_REQUIRES esp_timer freertos
)
