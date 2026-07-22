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
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_safeguard_reboot.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_snap.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system.c")
# bb_serialize: REQUIRES -- bb_system_snap.h is now a public header
# (B1-1025, floor's diag routes are its first real consumer) that itself
# includes bb_serialize.h (bb_serialize_desc_t), so bb_serialize must be
# public here too.
# app_update/spi_flash/esp_hw_support: HW-identity accessors relocated from
# bb_board (B1-977 dissolution) -- esp_ota_get_running_partition (app_update),
# esp_flash_get_size (spi_flash), esp_chip_info/esp_read_mac/esp_clk_cpu_freq
# (esp_hw_support).
set(_sys_priv_reqs bb_log bb_config esp_app_format bb_http esp_driver_tsens esp_system bb_ntp bb_json bb_str bb_storage app_update spi_flash esp_hw_support)

if(CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_routes.c")
endif()

if(CONFIG_BB_SYSTEM_BOOT_BANNER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_boot_banner.c")
endif()

idf_component_register(
    SRCS ${_sys_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server bb_serialize
    PRIV_REQUIRES ${_sys_priv_reqs}
)

# bb_system_routes.c is portable (no ESP-IDF-specific includes) but filed
# under platform/espidf/ and only added to _sys_srcs above when
# CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER is set -- the host scaffold's
# directory-convention source walk (scripts/bbtool/boards.py's
# derive_component) only looks under platform/host/<name>/ for the "native"
# board and has no CONFIG_ awareness, so this hint pulls the portable route
# source into host test builds too (same pattern as
# bb_storage_http/CMakeLists.txt's own hint).
# bbtool-scaffold-hint: source=platform/espidf/bb_system/bb_system_routes.c

