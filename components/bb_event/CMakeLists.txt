idf_component_register(
    SRCS "bb_event_common.c" "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_event/bb_event_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core
    PRIV_REQUIRES freertos bb_log bb_task bb_str)
