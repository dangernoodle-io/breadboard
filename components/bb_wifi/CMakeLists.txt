idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/wifi_reconn.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer esp_system bb_nv bb_log lwip bb_hw esp_app_format esp_http_server json
)
