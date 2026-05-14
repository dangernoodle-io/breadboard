idf_component_register(
    SRCS "src/bb_event_routes_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_event_routes/bb_event_routes_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_event bb_event_ring bb_core
    PRIV_REQUIRES bb_log bb_http bb_registry esp_http_server)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
if(CONFIG_BB_EVENT_ROUTES_AUTOREGISTER)
    bb_registry_force_register(${COMPONENT_LIB} bb_event_routes)
endif()
