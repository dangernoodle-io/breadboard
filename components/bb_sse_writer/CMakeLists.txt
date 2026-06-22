idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_sse_idle.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sse_writer/bb_sse_writer.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_http bb_log lwip freertos
)
