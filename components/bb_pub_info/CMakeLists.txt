idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_info/bb_pub_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub bb_board bb_system
    PRIV_REQUIRES bb_json bb_log bb_registry bb_diag bb_ntp bb_ota_validator esp_timer)

if(CONFIG_BB_PUB_INFO_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub_info)
endif()
