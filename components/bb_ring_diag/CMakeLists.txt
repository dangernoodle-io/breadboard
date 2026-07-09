idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ring_diag/bb_ring_diag.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ring_diag"
    REQUIRES bb_ring bb_core bb_http bb_http_server
    PRIV_REQUIRES bb_json esp_http_server bb_log bb_str)
