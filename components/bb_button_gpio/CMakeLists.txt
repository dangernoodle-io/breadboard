idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_button_gpio/bb_button_gpio.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_button
    PRIV_REQUIRES bb_log bb_task_registry esp_driver_gpio esp_timer)
