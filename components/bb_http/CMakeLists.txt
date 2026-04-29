idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http/bb_http.c"
         "src/http_utils.c"
         "src/route_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv bb_log bb_json esp_http_server
)
