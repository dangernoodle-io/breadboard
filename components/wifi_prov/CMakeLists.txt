idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/wifi_prov/wifi_prov.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/wifi_prov/wifi_reconn.c"
    INCLUDE_DIRS "include" "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/wifi_prov"
    REQUIRES esp_wifi esp_netif esp_event esp_timer esp_system nv_config log_stream lwip mdns board esp_app_format
)
