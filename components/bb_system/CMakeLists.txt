# bb_nv_namespaces.h's BB_REBOOT_NVS_NS / bb_nv_keys.h's
# BB_REBOOT_KEY_BOOT_CNT constants now live in bb_config (B1-958, bb_nv
# dissolution epic B1-708) -- bb_nv is no longer required at all here; the
# boot-health counter round-trips through bb_config below.
#
# bb_system_routes.c is now unconditionally listed (B1-1332: the
# BB_SYSTEM_ROUTES_AUTOREGISTER Kconfig gate that used to make this a
# conditional list(APPEND ...) is removed -- composition is codegen/handwire
# only, never a Kconfig-gated SRCS switch). It always compiles and its
# object is always archived into the component library; nothing strips it.
# Composition, not code presence, is the lever: a headless consumer that
# doesn't want POST /api/reboot simply doesn't name bb_system_routes_init in
# its manifest (or doesn't call it, hand-wired) -- same lever B1-1314/
# B1-1279 already rely on -- so the route is never registered/reachable,
# though the unreferenced object still carries a small flash cost.
set(_sys_srcs
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_reboot_record.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_boot_banner_format.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_boot_fail.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_reboot_budget.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_safeguard_reboot.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_restart_deferred.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_snap.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_routes.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_boot_banner.c")
# bb_serialize: REQUIRES -- bb_system_snap.h is now a public header
# (B1-1025, floor's diag routes are its first real consumer) that itself
# includes bb_serialize.h (bb_serialize_desc_t), so bb_serialize must be
# public here too.
# app_update/spi_flash/esp_hw_support: HW-identity accessors relocated from
# bb_board (B1-977 dissolution) -- esp_ota_get_running_partition (app_update),
# esp_flash_get_size (spi_flash), esp_chip_info/esp_read_mac/esp_clk_cpu_freq
# (esp_hw_support).
# bb_http_server: PRIV -- B1-1313: bb_http_handle_t/route declarations moved
# off bb_system.h onto the co-located include/bb_system_routes.h (bb_diag_http/
# bb_storage_http.h precedent), so bb_system's own public header no longer
# needs it; only bb_system_routes.c (this component's own .c, now always
# compiled -- see the composition-only note above _sys_srcs) still calls
# into it. A codegen consumer taking bb_system's primitives (reboot-reason
# SSOT, boot banner, boot-fail counters, bb_system_snap) no longer inherits
# this edge unless it separately includes bb_system_routes.h.
set(_sys_priv_reqs bb_log bb_config esp_app_format bb_http esp_driver_tsens esp_system bb_ntp bb_data bb_str bb_storage app_update spi_flash esp_hw_support bb_timer bb_http_server)

# bb_serialize_meta: PUBLIC -- include/bb_system.h includes
# bb_serialize_meta.h directly (co-located bb_serialize_desc_meta_t table,
# gated on BB_SERIALIZE_META_SHIP; B1-1059 device-shippable PR, default off
# via CONFIG_BB_OPENAPI_RUNTIME_META); PRIV_REQUIRES doesn't propagate to a
# hand-wired external consumer of this public header.
idf_component_register(
    SRCS ${_sys_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_serialize bb_serialize_meta
    PRIV_REQUIRES ${_sys_priv_reqs}
)

# bb_system_routes.c is portable (no ESP-IDF-specific includes) but filed
# under platform/espidf/ and unconditionally added to _sys_srcs above -- the
# host scaffold's directory-convention source walk (scripts/bbtool/boards.py's
# derive_component) only looks under platform/host/<name>/ for the "native"
# board, so this hint pulls the portable route source into host test builds
# too (same pattern as bb_diag_http/CMakeLists.txt's own hint, carried over
# from the dissolved bb_storage_http component, B1-1154).
# bbtool-scaffold-hint: source=platform/espidf/bb_system/bb_system_routes.c

