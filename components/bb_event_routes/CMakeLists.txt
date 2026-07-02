idf_component_register(
    SRCS "src/bb_event_routes_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_event_routes/bb_event_topic_registry.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_event bb_event_ring bb_core
    PRIV_REQUIRES bb_log bb_registry bb_arena)
