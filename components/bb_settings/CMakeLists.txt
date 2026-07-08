idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_settings/bb_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_config bb_core
)

# bb_settings is a portable provider over bb_config (no ESP-IDF deps) --
# compiles unchanged on host and ESP-IDF, filed under platform/host/ only,
# mirroring bb_config and bb_dispatch_cmd.
# bbtool-scaffold-hint: source=platform/host/bb_settings/bb_settings.c
