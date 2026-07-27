idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_parse.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/bb_wifi_prov.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/wifi_prov_mgr.c"
         "${CMAKE_CURRENT_LIST_DIR}/wifi_prov_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_settings bb_fsm bb_wifi_ap bb_task bb_wifi
)
