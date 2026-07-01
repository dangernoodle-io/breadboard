idf_component_register(
    SRCS "bb_event_common.c" "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_event/bb_event_espidf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core
    PRIV_REQUIRES freertos bb_log bb_init bb_task_registry)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_EVENT_AUTOREGISTER)
    bb_init_force_register_early(${COMPONENT_LIB} bb_event)
endif()
