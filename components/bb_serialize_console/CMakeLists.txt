idf_component_register(
    SRCS "src/bb_serialize_console.c" "src/bb_serialize_console_format.c"
         "src/bb_serialize_console_heap.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_serialize bb_core
    PRIV_REQUIRES bb_meminfo bb_log bb_num
)
