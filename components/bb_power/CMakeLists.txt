idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_power/bb_power.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_log)

# include/ exposes bb_power_driver.h (semi-private driver vtable interface).
# Power driver components (e.g. bb_power_tps546) REQUIRES bb_power to access it.
