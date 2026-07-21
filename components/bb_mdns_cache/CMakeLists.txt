# bb_serialize: PRIVATE -- bb_mdns_cache_wire_priv.h (the "mdns_cache entry"
# wire descriptor, B1-1115 PR-3) includes bb_serialize.h, but is a
# component-root private header, never exposed by include/bb_mdns_cache.h
# (mirrors bb_wifi_http's own bb_wifi_http_wire_priv.h REQUIRES split).
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mdns_cache/bb_mdns_cache_entry.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mdns_cache/bb_mdns_cache_txt.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_mdns_cache/bb_mdns_cache_validate.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mdns_cache/bb_mdns_cache.c"
        "${CMAKE_CURRENT_LIST_DIR}/bb_mdns_cache_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_mdns
    PRIV_REQUIRES bb_cache bb_timer bb_log espressif__mdns esp_netif esp_system bb_str bb_serialize
)
