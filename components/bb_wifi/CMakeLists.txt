idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/wifi_reconn.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi/bb_wifi_gw_probe.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi/bb_wifi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/wifi_reconn_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core esp_netif
    PRIV_REQUIRES bb_alert bb_init bb_wdt bb_task bb_str bb_settings esp_wifi esp_event esp_timer esp_system bb_log bb_nv lwip bb_timer bb_system
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")

if(CONFIG_BB_WIFI_AUTOREGISTER)
    bb_init_force_register_early(${COMPONENT_LIB} bb_wifi)
endif()

if(CONFIG_BB_WIFI_GW_PROBE_ENABLE)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_wifi_gw_probe)
endif()
