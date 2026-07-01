idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mdns/bb_mdns.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_util.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_lifecycle.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core
    PRIV_REQUIRES bb_http bb_log bb_hw bb_wifi bb_init bb_timer bb_ring esp_app_format esp_netif espressif__mdns esp_system
)

# Ensure managed component is available
if(DEFINED IDF_COMPONENT_MANAGER AND IDF_COMPONENT_MANAGER)
    # Will be fetched via idf_component.yml
endif()

if(CONFIG_BB_MDNS_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_mdns)
endif()
