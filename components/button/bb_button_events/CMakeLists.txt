idf_component_register(
    SRCS "../../../platform/host/bb_button_events/bb_button_events.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_button
    PRIV_REQUIRES bb_log bb_timer)
