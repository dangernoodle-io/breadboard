idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_http/bb_wifi_http_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi_http/bb_wifi_http_emit.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_wifi bb_json
    PRIV_REQUIRES bb_cache bb_http bb_http_server bb_openapi bb_init bb_nv esp_wifi
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")

if(CONFIG_BB_WIFI_ROUTES_AUTOREGISTER)
    # Legacy symbol bb_wifi_routes (not bb_wifi_http_routes) maintains di-fence marker baseline
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_wifi_routes)
    bb_init_force_register(${COMPONENT_LIB} bb_wifi_routes)
endif()
