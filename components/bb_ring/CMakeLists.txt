idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_ring/bb_ring.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_ring/bb_ring_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_registry)
