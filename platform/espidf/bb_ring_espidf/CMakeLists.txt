idf_component_register(
    SRCS "bb_ring_spiram.c"
    REQUIRES bb_ring
    PRIV_REQUIRES bb_registry)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bb_registry.cmake")
bb_registry_force_register_early(${COMPONENT_LIB} bb_ring_spiram)
