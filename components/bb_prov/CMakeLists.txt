idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_prov_parse.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_prov/bb_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES http_server wifi_prov nv_config log_stream esp_http_server
)
