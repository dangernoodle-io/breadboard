idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_display/bb_sink_display_select.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_display/bb_sink_display_format.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_display/bb_sink_display_table.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_display/bb_sink_display_validate.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sink_display/bb_sink_display.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_attrs bb_filter bb_json
    PRIV_REQUIRES bb_cache bb_cache_reactive bb_display bb_timer bb_log)
