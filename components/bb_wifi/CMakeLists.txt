idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/wifi_reconn.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi/bb_wifi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/wifi_reconn_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_http bb_registry bb_ota_validator bb_wdt esp_wifi esp_netif esp_event esp_timer esp_system bb_log bb_nv lwip bb_timer
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")

if(CONFIG_BB_WIFI_AUTOREGISTER)
    bb_registry_force_register_early(${COMPONENT_LIB} bb_wifi)
endif()

if(CONFIG_BB_WIFI_ROUTES_AUTOREGISTER)
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_wifi_routes)
    bb_registry_force_register(${COMPONENT_LIB} bb_wifi_routes)
endif()
