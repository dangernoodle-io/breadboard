idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_led/bb_led.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log)

# include/ exposes bb_led_driver.h (semi-private driver vtable interface).
# LED driver components (e.g. bb_led_ws2812) REQUIRES bb_led to access it.
