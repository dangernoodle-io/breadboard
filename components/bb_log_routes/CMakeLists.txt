idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_routes/bb_log_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_log_routes/bb_log_http.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_log bb_nv bb_http bb_json
)
