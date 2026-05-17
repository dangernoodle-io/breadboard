idf_component_register(
    SRCS "src/bb_update_check_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_update_check/bb_update_check_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_release_manifest
    PRIV_REQUIRES bb_http_client bb_event bb_http bb_log bb_mdns bb_nv bb_system bb_timer bb_registry bb_json esp_http_server bb_event_routes)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
if(CONFIG_BB_UPDATE_CHECK_AUTOREGISTER)
    bb_registry_force_register(${COMPONENT_LIB} bb_update_check)
endif()
