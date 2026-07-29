idf_component_register(
    SRCS "src/bb_serialize_console.c" "src/bb_serialize_console_format.c"
         "src/bb_serialize_console_heap.c" "src/bb_serialize_console_tasks.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_serialize bb_core
    # bb_task: PRIV -- bb_serialize_console_tasks.c's gather reads
    # bb_task_base_foreach()/BB_TASK_BASE_MAX_CAP only (.c use), the SSOT
    # per-task stack-free-bytes reader persisted by the periodic base-scan
    # (no direct FreeRTOS task-state calls here).
    PRIV_REQUIRES bb_meminfo bb_log bb_num bb_task bb_str
)
