idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ntp_info/bb_ntp_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_ntp bb_info bb_core
    PRIV_REQUIRES bb_log bb_json)
