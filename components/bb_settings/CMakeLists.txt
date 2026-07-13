idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_settings/bb_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_config bb_core
    PRIV_REQUIRES bb_storage
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
# bbtool-scaffold-hint: source=platform/host/bb_settings/bb_settings.c
