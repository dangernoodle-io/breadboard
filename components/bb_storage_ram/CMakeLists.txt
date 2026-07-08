idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_storage_ram/bb_storage_ram.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_storage bb_str bb_log
)

# bb_storage_ram is a portable backend (no ESP-IDF deps) -- compiles unchanged
# on host and ESP-IDF, filed under platform/host/ only, mirroring bb_storage.
# bbtool-scaffold-hint: source=platform/host/bb_storage_ram/bb_storage_ram.c
