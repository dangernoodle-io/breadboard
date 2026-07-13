set(BB_TCP_CLIENT_PRIV_REQUIRES bb_nv bb_log bb_transport_health)

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
    REQUIRES bb_core
    PRIV_REQUIRES ${BB_TCP_CLIENT_PRIV_REQUIRES})
