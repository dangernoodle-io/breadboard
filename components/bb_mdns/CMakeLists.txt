idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mdns/bb_mdns.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_util.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_lifecycle.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES espressif__mdns esp_system app_update bb_core
    PRIV_REQUIRES bb_http bb_log bb_hw bb_wifi bb_registry
)

# Ensure managed component is available
if(DEFINED IDF_COMPONENT_MANAGER AND IDF_COMPONENT_MANAGER)
    # Will be fetched via idf_component.yml
endif()

if(CONFIG_BB_MDNS_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_mdns)
endif()
