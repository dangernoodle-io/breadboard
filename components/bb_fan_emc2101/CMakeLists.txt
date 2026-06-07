idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_fan_emc2101/bb_fan_emc2101.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_fan esp_driver_i2c
    PRIV_REQUIRES bb_log)
