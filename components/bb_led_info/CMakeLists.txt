idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_led_info/bb_led_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_led bb_info bb_core
    PRIV_REQUIRES bb_log bb_json bb_init bb_http_server)

if(CONFIG_BB_LED_INFO_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_led_info)
endif()
