idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_heap_arena/bb_heap_arena.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES mbedtls heap
)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")

if(CONFIG_BB_HEAP_ARENA_AUTOREGISTER)
    bb_registry_force_register_early(${COMPONENT_LIB} bb_heap_arena)
endif()
