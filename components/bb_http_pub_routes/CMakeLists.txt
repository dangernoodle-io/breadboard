idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http_pub_routes/bb_http_pub_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http_pub bb_core
    PRIV_REQUIRES bb_http bb_log bb_registry bb_json bb_nv bb_tls_creds)

if(CONFIG_BB_HTTP_PUB_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_http_pub_routes)
endif()
