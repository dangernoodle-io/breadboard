idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_ota_led/bb_ota_led.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core)
