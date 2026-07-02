idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_arena_tls/bb_arena_tls.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_arena mbedtls
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")

if(CONFIG_BB_ARENA_TLS_AUTOREGISTER)
    bb_init_force_register_early(${COMPONENT_LIB} bb_arena_tls)
endif()
