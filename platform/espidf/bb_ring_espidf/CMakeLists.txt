idf_component_register(
    SRCS "bb_ring_spiram.c"
    REQUIRES bb_ring
    PRIV_REQUIRES bb_init)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bb_init.cmake")
bb_init_force_register_early(${COMPONENT_LIB} bb_ring_spiram)
