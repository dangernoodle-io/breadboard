idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_display_info/bb_display_info.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_display_info_event_common.c"
    INCLUDE_DIRS "include" "."
    REQUIRES bb_display bb_info bb_nv bb_core
    PRIV_REQUIRES bb_log bb_json bb_event bb_event_routes bb_openapi bb_init bb_cache)

if(CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_display_info)
endif()
