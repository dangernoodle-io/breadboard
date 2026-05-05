idf_component_register(
    SRCS "../../platform/espidf/bb_led_anim/bb_led_anim.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_led bb_log bb_timer esp_timer)
