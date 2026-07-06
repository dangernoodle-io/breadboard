idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mdns_cache/bb_mdns_cache_entry.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mdns_cache/bb_mdns_cache_txt.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mdns_cache/bb_mdns_cache_validate.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mdns_cache/bb_mdns_cache.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_mdns
    PRIV_REQUIRES bb_cache bb_cache_reactive bb_timer bb_log bb_init espressif__mdns esp_netif esp_system bb_str
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_MDNS_CACHE_AUTOREGISTER)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_mdns_cache)
endif()
