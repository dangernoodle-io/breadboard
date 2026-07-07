set(BB_SINK_UDP_PRIV_REQUIRES bb_log bb_init)

if(IDF_TARGET)
    set(BB_SINK_UDP_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sink_udp/bb_sink_udp.c")
else()
    set(BB_SINK_UDP_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_udp/bb_sink_udp.c")
endif()

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_sink_udp_common.c"
        "${BB_SINK_UDP_BACKEND}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_pub bb_udp_client bb_udp_frame
    PRIV_REQUIRES ${BB_SINK_UDP_PRIV_REQUIRES})

if(CONFIG_BB_SINK_UDP_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_sink_udp)
endif()
