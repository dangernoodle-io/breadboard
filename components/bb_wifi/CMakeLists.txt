idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/wifi_reconn.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi/bb_wifi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/wifi_reconn_policy.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_pending.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_wifi_link_classify.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core
    PRIV_REQUIRES bb_task bb_str bb_settings esp_wifi esp_event esp_system bb_log lwip bb_timer bb_system esp_netif bb_fsm
)
