# bb_nv stays PRIV_REQUIRES for bb_nv_namespaces.h's BB_UDP_NVS_NS constant
# only (B1-756) -- host/port/broadcast no longer round-trip through bb_nv's
# generic-KV API, that logic now lives in bb_config below.
set(BB_UDP_CLIENT_PRIV_REQUIRES bb_nv bb_config bb_log)

if(IDF_TARGET)
    set(BB_UDP_CLIENT_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_udp_client/bb_udp_client.c")
    list(APPEND BB_UDP_CLIENT_PRIV_REQUIRES lwip)
else()
    set(BB_UDP_CLIENT_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_udp_client/bb_udp_client.c")
endif()

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_udp_client_common.c"
        "${BB_UDP_CLIENT_BACKEND}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core
    PRIV_REQUIRES ${BB_UDP_CLIENT_PRIV_REQUIRES})
