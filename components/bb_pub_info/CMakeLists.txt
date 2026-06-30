idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_pub_info/bb_pub_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub
    # bb_ota_pull is for the ota_ready field; emission is gated in bb_pub_info.c
    # (BB_PUB_INFO_EMIT_OTA_READY) so --gc-sections drops it on boards with no
    # runtime OTA-TLS path. (bb_info already requires bb_ota_pull, so this adds no
    # new system-level dep.) REQUIRES can't be CONFIG-conditional in ESP-IDF.
    # bb_board is PRIV: no bb_board types appear in bb_pub_info.h; it is used
    # only in bb_pub_info.c.
    PRIV_REQUIRES bb_board bb_json bb_log bb_openapi bb_registry bb_diag bb_ntp bb_ota_validator bb_ota_pull)

if(CONFIG_BB_PUB_INFO_AUTO_ATTACH)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_pub_info)
endif()
