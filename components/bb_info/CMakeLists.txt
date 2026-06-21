idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_info/bb_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv bb_json bb_section
    PRIV_REQUIRES bb_http bb_board bb_wifi bb_registry bb_log bb_ntp bb_mdns esp_timer bb_ota_pull
)

if(CONFIG_BB_INFO_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_info)
    bb_registry_force_register(${COMPONENT_LIB} bb_info)
    bb_registry_force_register(${COMPONENT_LIB} bb_info_freeze)
endif()
