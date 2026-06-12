idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_tls_creds/bb_tls_creds.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_nv bb_log)
