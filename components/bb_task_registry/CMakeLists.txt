idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_task_registry/bb_task_registry.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_task_registry/bb_task_registry_monitor.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_task_registry_base_scan_common.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_task_registry/bb_task_registry_base_scan.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    # bb_num: PRIV -- B1-1256: bb_num_words_to_bytes() call in
    # bb_task_registry_base_scan.c's do_scan() (internal .c use only) --
    # the shared SSOT stack-words-to-bytes conversion, also used by
    # bb_diag_http's GET /api/diag/tasks gather.
    PRIV_REQUIRES bb_log bb_registry bb_wdt esp_system bb_task bb_timer bb_str bb_num)
