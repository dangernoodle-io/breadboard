idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_nv_wifi_pending.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    # bb_system PRIV_REQUIRES bb_nv (generic KV) — this reverse edge is a
    # 2-node PRIV_REQUIRES-only cycle, needed only for bb_nv_routes.c's
    # factory-reset path to call bb_system_reboot_record_save (B1-750, the
    # reboot-record duty moved to bb_system, which already owns the NVS
    # namespace/keys). ESP-IDF's build system resolves small PRIV_REQUIRES
    # cycles like this automatically (repeats the lib on the linker line).
    PRIV_REQUIRES bb_log bb_manifest bb_http bb_http_server bb_json nvs_flash bb_timer bb_str bb_storage_nvs bb_storage_rtc bb_system
)

# bb_nv has no platform/host/bb_nv/ — its core impl + routes are portable
# (guarded by #ifdef ESP_PLATFORM internally) but filed under platform/espidf/
# only; the host build needs them (and the private headers colocated there).
# bbtool-scaffold-hint: source=platform/espidf/bb_nv/bb_nv.c
# bbtool-scaffold-hint: source=platform/espidf/bb_nv/bb_nv_routes.c
# bbtool-scaffold-hint: include=platform/espidf/bb_nv
