idf_component_register(
    SRCS "../../platform/espidf/bb_button_events/bb_button_events.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_button bb_log esp_timer)
