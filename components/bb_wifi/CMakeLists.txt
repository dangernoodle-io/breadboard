idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/wifi_reconn.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core esp_wifi esp_netif esp_event esp_timer esp_system bb_nv bb_log lwip
    PRIV_REQUIRES bb_http bb_json bb_registry
)

if(CONFIG_BB_WIFI_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_wifi_routes)
endif()
