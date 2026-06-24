idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../bb_event_routes/bb_event_routes_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_event_routes_spiram.c"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/bb_event_routes/src"
    REQUIRES bb_event_routes bb_event bb_event_ring bb_core bb_http
    PRIV_REQUIRES bb_registry bb_json esp_http_server bb_log bb_sse_writer bb_timer)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bb_registry.cmake")
if(CONFIG_BB_EVENT_ROUTES_AUTOREGISTER)
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_event_routes)
    bb_registry_force_register(${COMPONENT_LIB} bb_event_routes)
endif()
