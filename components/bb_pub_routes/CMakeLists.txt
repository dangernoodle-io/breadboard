idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_pub_routes/bb_pub_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_pub bb_core
    PRIV_REQUIRES bb_http bb_json bb_registry bb_log esp_timer)

if(CONFIG_BB_PUB_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_pub_routes)
endif()
