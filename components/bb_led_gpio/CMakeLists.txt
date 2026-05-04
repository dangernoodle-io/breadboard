idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_led_gpio/bb_led_gpio.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_led esp_driver_gpio
    PRIV_REQUIRES bb_log)
