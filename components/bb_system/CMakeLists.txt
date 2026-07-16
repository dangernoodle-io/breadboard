# bb_nv_namespaces.h's BB_REBOOT_NVS_NS / bb_nv_keys.h's
# BB_REBOOT_KEY_BOOT_CNT constants now live in bb_config (B1-958, bb_nv
# dissolution epic B1-708) -- bb_nv is no longer required at all here; the
# boot-health counter round-trips through bb_config below.
set(_sys_srcs
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_reboot_parse.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_reboot_record.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_boot_banner_format.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_boot_fail.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_reboot_budget.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system.c")
set(_sys_priv_reqs bb_log bb_config esp_app_format bb_http esp_driver_tsens esp_system bb_ntp bb_json bb_str bb_storage)

if(CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_routes.c")
endif()

if(CONFIG_BB_SYSTEM_BOOT_BANNER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_boot_banner.c")
endif()

idf_component_register(
    SRCS ${_sys_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES ${_sys_priv_reqs}
)

