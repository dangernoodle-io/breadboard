idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_temp/bb_temp.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_health bb_json bb_core
    PRIV_REQUIRES bb_log bb_system bb_init)

if(CONFIG_BB_TEMP_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_temp)
endif()
