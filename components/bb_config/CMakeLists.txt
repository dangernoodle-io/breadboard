idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_config/bb_config.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_config/bb_config_staged.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_storage bb_core
)

# bb_config is a portable typed layer over bb_storage (no ESP-IDF deps) --
# compiles unchanged on host and ESP-IDF, filed under platform/host/ only,
# mirroring bb_storage and bb_dispatch_cmd. bb_config_staged.c is a second TU
# in this component: a stateful staged-write session over the same typed
# layer, reusing bb_config's own encode path -- it adds no dependency beyond
# bb_storage/bb_core (its txn vtable already lives inside bb_storage), so it
# stays a sub-namespace here rather than a separate component.
# bbtool-scaffold-hint: source=platform/host/bb_config/bb_config.c
# bbtool-scaffold-hint: source=platform/host/bb_config/bb_config_staged.c
