idf_component_register(
    SRCS "bb_event_ring_clock.c"
         "bb_event_ring_spiram.c"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/bb_event_ring"
    REQUIRES bb_event_ring esp_timer bb_registry)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bb_registry.cmake")
bb_registry_force_register_early(${COMPONENT_LIB} bb_event_ring_spiram)
