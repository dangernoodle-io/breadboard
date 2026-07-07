idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_task_common.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_task/bb_task_espidf.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_registry freertos bb_str
)
