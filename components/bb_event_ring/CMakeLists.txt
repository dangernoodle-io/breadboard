idf_component_register(
    SRCS "bb_event_ring.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_event bb_core
    PRIV_REQUIRES bb_log)
