idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_info/bb_info.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_nv bb_json
    PRIV_REQUIRES bb_http bb_board bb_wifi bb_registry
)

if(CONFIG_BB_INFO_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_info)
endif()
