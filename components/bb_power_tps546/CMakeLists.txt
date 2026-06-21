idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_power_tps546/bb_power_tps546.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_power_tps546/bb_power_tps546_program.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_power esp_driver_i2c
    PRIV_REQUIRES bb_core bb_log esp_timer)
