# bb_serialize: PRIV_REQUIRES -- bb_wifi_prov_scan_wire.c (POST /api/wifi/scan
# response, B1-809 portal) uses bb_serialize_field_t/bb_serialize_desc_t
# directly. Already reachable transitively via bb_http_server (whose public
# bb_http_serialize_stream.h REQUIRES bb_serialize_json, which itself
# REQUIRES bb_serialize) -- listed explicitly per this repo's direct-deps-only
# convention rather than relying on that transitive path.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_parse.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_scan_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/bb_wifi_prov.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/wifi_prov_mgr.c"
         "${CMAKE_CURRENT_LIST_DIR}/wifi_prov_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_settings bb_fsm bb_wifi_ap bb_task bb_wifi bb_serialize
)
