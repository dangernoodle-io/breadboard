idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_settings/bb_settings.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_settings_creds_boot_decide.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_config bb_core
    PRIV_REQUIRES bb_storage bb_manifest bb_storage_nvs bb_log
)

# bb_settings is a portable provider over bb_config (no ESP-IDF deps) --
# compiles unchanged on host and ESP-IDF, filed under platform/host/ only,
# mirroring bb_config and bb_dispatch_cmd.
#
# bb_storage is PRIV_REQUIRES: the pending-creds writers' .c uses
# bb_config_staged_*() (atomic multi-field commit, now a bb_config
# sub-namespace -- see REQUIRES bb_config above) and bb_storage_erase()
# (best-effort pending-bytes cleanup) directly, but neither is exposed in
# bb_settings.h's public signatures.
#
# bb_manifest/bb_storage_nvs/bb_log are PRIV_REQUIRES for
# bb_settings_creds_boot_init/_manifest_init (B1-963/B1-708: relocated
# verbatim from bb_nv_config_init/_manifest_init) -- bb_manifest_register_nv,
# bb_storage_nvs_flash_init, and bb_log_{w,e,i} are called from
# bb_settings.c but none of their types appear in bb_settings.h's public
# signatures. bb_settings_creds_boot_decide.c is a private pure-decision
# helper (src/, mirroring bb_storage_nvs_get_decision's PRIV_INCLUDE_DIRS
# "src" pattern) -- moved+renamed from bb_nv's own
# components/bb_nv/src/bb_nv_creds_boot_decide.{c,h}.
# bbtool-scaffold-hint: source=platform/host/bb_settings/bb_settings.c
# bbtool-scaffold-hint: source=components/bb_settings/src/bb_settings_creds_boot_decide.c
