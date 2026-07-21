# bb_meminfo — canonical system-heap reader SSOT (KB #698/#699/#693).
# The ONE component that calls heap_caps_*. Also owns the heap-state
# classifier relocated from bb_board (B1-977 dissolution). This is a
# pure on-demand reader: no routes, no pub/sink glue, no BB_INIT_REGISTER hook.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_meminfo/bb_meminfo.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_meminfo_heap_snap.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_meminfo_heap_state.c"
    INCLUDE_DIRS "include"
    # bb_serialize: REQUIRES -- bb_meminfo_heap_snap.h is now a public header
    # (B1-1025, floor's diag routes are its first real consumer) that itself
    # includes bb_serialize.h (bb_serialize_desc_t), so bb_serialize must be
    # public here too.
    REQUIRES bb_core bb_mem_arena bb_serialize
    PRIV_REQUIRES bb_registry
)
