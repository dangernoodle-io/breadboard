idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_info/bb_display_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_display bb_info bb_nv bb_core
    PRIV_REQUIRES bb_log bb_json)
