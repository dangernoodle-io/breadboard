include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_tls_info/bb_tls_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_info bb_core
    PRIV_REQUIRES bb_init bb_log)

bb_init_force_register_pre_http(${COMPONENT_LIB} bb_tls_info)
