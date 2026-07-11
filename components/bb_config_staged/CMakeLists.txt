idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_config_staged/bb_config_staged.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_config bb_storage bb_core
)

# bb_config_staged is a portable staged-write primitive over bb_config/
# bb_storage (no ESP-IDF deps) -- compiles unchanged on host and ESP-IDF,
# filed under platform/host/ only, mirroring bb_config and bb_settings.
# bbtool-scaffold-hint: source=platform/host/bb_config_staged/bb_config_staged.c
