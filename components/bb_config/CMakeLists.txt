idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_config/bb_config.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_storage bb_core
)

# bb_config is a portable typed layer over bb_storage (no ESP-IDF deps) --
# compiles unchanged on host and ESP-IDF, filed under platform/host/ only,
# mirroring bb_storage and bb_dispatch_cmd.
# bbtool-scaffold-hint: source=platform/host/bb_config/bb_config.c
