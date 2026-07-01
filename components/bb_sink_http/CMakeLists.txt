idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_http/bb_sink_http.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub bb_tls
    PRIV_REQUIRES bb_http_client bb_tls_creds bb_board bb_nv bb_json bb_log bb_init)
