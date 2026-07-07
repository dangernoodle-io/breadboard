idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ring_diag/bb_ring_diag.c"
    REQUIRES bb_ring bb_core bb_http bb_http_server
    PRIV_REQUIRES bb_init bb_json esp_http_server bb_log bb_str)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_RING_DIAG_AUTOREGISTER)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_ring_diag)
    bb_init_force_register(${COMPONENT_LIB} bb_ring_diag)
endif()
