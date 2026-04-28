idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES bb_log bb_registry
)

if(CONFIG_BB_NV_FLASH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_early(${COMPONENT_LIB} bb_nv_flash)
endif()

if(CONFIG_BB_NV_CONFIG_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register_early(${COMPONENT_LIB} bb_nv_config)
endif()
