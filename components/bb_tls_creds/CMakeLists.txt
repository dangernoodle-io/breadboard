# bb_nv stays PRIV_REQUIRES for bb_nv_keys.h's BB_NV_KEY_TLS_CA/CERT/KEY
# constants only (B1-756) -- NVS reads no longer go through bb_nv's
# generic-KV API, that logic now lives in bb_config below.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_tls_creds/bb_tls_creds.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_nv bb_config bb_log)
