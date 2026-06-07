set(_led_srcs
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_led/bb_led.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_led/bb_led_gamma.c")
set(_led_reqs bb_core)
set(_led_priv_reqs bb_log)

idf_component_register(
    SRCS ${_led_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${_led_reqs}
    PRIV_REQUIRES ${_led_priv_reqs})

# include/ exposes bb_led_driver.h (semi-private driver vtable interface).
# LED driver components (e.g. bb_led_ws2812) REQUIRES bb_led to access it.
