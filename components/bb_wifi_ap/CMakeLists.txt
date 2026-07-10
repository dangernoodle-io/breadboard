idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_ap_core.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_ap/bb_wifi_ap.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_str bb_task esp_wifi esp_netif esp_event lwip
)
