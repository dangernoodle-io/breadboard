idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_storage/bb_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log
)

# bb_storage is a portable facade (no ESP-IDF deps, guarded internally by
# nothing platform-specific) — its impl compiles unchanged on host and
# ESP-IDF, so it is filed under platform/host/ only, mirroring bb_dispatch_cmd.
# bbtool-scaffold-hint: source=platform/host/bb_storage/bb_storage.c
