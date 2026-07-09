idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mdns/bb_mdns.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_util.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_lifecycle.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_refresh_decision.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_hw bb_wifi bb_timer bb_ring bb_task esp_app_format esp_netif espressif__mdns esp_system bb_str
)

# Ensure managed component is available
if(DEFINED IDF_COMPONENT_MANAGER AND IDF_COMPONENT_MANAGER)
    # Will be fetched via idf_component.yml
endif()
