# bb_meminfo — canonical system-heap reader SSOT (KB #698/#699/#693).
# The ONE component that calls heap_caps_*. bb_board's heap_/psram_/rtc_
# accessors delegate here (thin wrappers, unchanged public API). This is a
# pure on-demand reader: no routes, no pub/sink glue, no BB_INIT_REGISTER hook.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_meminfo/bb_meminfo.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_mem_arena
    PRIV_REQUIRES bb_registry
)
