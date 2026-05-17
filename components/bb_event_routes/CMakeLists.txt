idf_component_register(
    SRCS "src/bb_event_routes_common.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_event bb_event_ring bb_core
    PRIV_REQUIRES bb_log)
