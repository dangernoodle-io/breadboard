idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_fan/bb_fan.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log)

# include/ exposes bb_fan_driver.h (semi-private driver vtable interface).
# Fan driver components (e.g. bb_fan_emc2101) REQUIRES bb_fan to access it.
