idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mem_arena_tls/bb_mem_arena_tls.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_mem_arena mbedtls
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")

if(CONFIG_BB_MEM_ARENA_TLS_AUTOREGISTER)
    bb_init_force_register_early(${COMPONENT_LIB} bb_mem_arena_tls)
endif()
