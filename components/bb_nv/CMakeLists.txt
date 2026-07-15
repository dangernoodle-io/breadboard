idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log nvs_flash bb_str bb_storage_nvs bb_settings bb_config
)

# bb_nv_namespaces.h / bb_nv_keys.h moved to bb_config (B1-958, bb_nv
# dissolution epic B1-708) -- they were already neutral cross-component SSOT
# headers (namespace/key strings owned by bb_system, bb_net_health, etc.,
# not bb_nv) that just physically lived here; bb_nv.c still needs
# BB_NV_CONFIG_NVS_NS for its own NVS reads, hence PRIV_REQUIRES bb_config.

# bb_nv has no platform/host/bb_nv/ — its core impl is portable (guarded by
# #ifdef ESP_PLATFORM internally) but filed under platform/espidf/ only; the
# host build needs it (and the private headers colocated there).
#
# B1-963: bb_manifest dropped from PRIV_REQUIRES and components/bb_nv/src/
# removed -- bb_nv_config_manifest_init (its only bb_manifest_* caller) and
# bb_nv_creds_boot_decide.{c,h} (the only src/ file) both relocated to
# bb_settings. bb_nv now has ZERO live functions remaining (B1-964 is a pure
# delete).
# bbtool-scaffold-hint: source=platform/espidf/bb_nv/bb_nv.c
# bbtool-scaffold-hint: include=platform/espidf/bb_nv
