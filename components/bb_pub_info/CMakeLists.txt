idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_info/bb_pub_info.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_pub_info_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub
    # bb_ota_pull is for the ota_ready field; emission is gated in bb_pub_info.c
    # (BB_PUB_INFO_EMIT_OTA_READY) so --gc-sections drops it on boards with no
    # runtime OTA-TLS path. REQUIRES can't be CONFIG-conditional in ESP-IDF.
    # bb_board is PRIV: no bb_board types appear in bb_pub_info.h; it is used
    # only in bb_pub_info.c.
    # bb_serialize: PRIV_REQUIRES -- bb_pub_info_wire_priv.h (v2 wire descriptor,
    # B1-767 PR-6) is a private header, not exposed under include/.
    PRIV_REQUIRES bb_board bb_json bb_log bb_openapi bb_diag bb_ntp bb_ota_validator bb_ota_pull bb_serialize)
