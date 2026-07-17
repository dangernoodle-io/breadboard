idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_parse.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/bb_wifi_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server bb_wifi
    PRIV_REQUIRES bb_log bb_settings
)
