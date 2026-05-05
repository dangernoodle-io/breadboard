idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_button/bb_button.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log)

# include/ exposes bb_button_driver.h (semi-private driver vtable interface).
# Button driver components (e.g. bb_button_gpio) REQUIRES bb_button to access it.
