idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_prov_parse.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_prov/bb_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http bb_wifi bb_nv bb_log esp_http_server esp_wifi esp_netif lwip
)
