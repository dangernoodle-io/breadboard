idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../platform/espidf/bb_led_apa102/bb_led_apa102.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_led
    PRIV_REQUIRES bb_log esp_driver_gpio)
