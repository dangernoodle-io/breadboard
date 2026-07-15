# bb_nv_keys.h's BB_NV_KEY_TLS_CA/CERT/KEY constants now live in bb_config
# (B1-958, bb_nv dissolution epic B1-708) -- bb_nv is no longer required at
# all here; NVS reads round-trip through bb_config below.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_tls_creds/bb_tls_creds.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_config bb_log)
