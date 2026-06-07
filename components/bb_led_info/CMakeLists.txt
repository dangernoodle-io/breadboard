idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_led_info/bb_led_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_led bb_info bb_json bb_core
    PRIV_REQUIRES bb_log)
