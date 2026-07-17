# bb_nv dropped (B1-951): ns is now caller-supplied, so this component no
# longer references bb_nv_namespaces.h's BB_TCP_NVS_NS (or any other bb_nv
# symbol) -- host/port/tls round-trip through bb_config below only.
#
# bb_transport_health dropped (B1-1039): health is now per-instance (see
# bb_tcp_client_priv.h) -- no shared/authoritative transport-health slot.
set(BB_TCP_CLIENT_PRIV_REQUIRES bb_config bb_log)

if(IDF_TARGET)
    set(BB_TCP_CLIENT_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_tcp_client/bb_tcp_client.c")
    list(APPEND BB_TCP_CLIENT_PRIV_REQUIRES esp-tls tcp_transport lwip)
else()
    set(BB_TCP_CLIENT_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_tcp_client/bb_tcp_client.c")
endif()

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_tcp_client_common.c"
        "${BB_TCP_CLIENT_BACKEND}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # bb_serialize: REQUIRES -- bb_tcp_client.h is a public header that
    # itself includes bb_serialize.h (bb_serialize_desc_t), so bb_serialize
    # must be public here too (mirrors bb_meminfo's bb_meminfo_heap_snap.h).
    REQUIRES bb_core bb_serialize
    PRIV_REQUIRES ${BB_TCP_CLIENT_PRIV_REQUIRES})
