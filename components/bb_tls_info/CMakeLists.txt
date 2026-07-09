idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_tls_info/bb_tls_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_info bb_core
    PRIV_REQUIRES bb_log)
