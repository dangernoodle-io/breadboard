idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_nv/bb_nv_delete_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_nv_creds_mirror.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_nv_wifi_pending.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_nv_reboot.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_init bb_manifest bb_http bb_json nvs_flash bb_timer bb_str
)

if(CONFIG_BB_NV_FLASH_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_nv_flash)
endif()

if(CONFIG_BB_NV_CONFIG_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_nv_config)
endif()

if(CONFIG_BB_NV_CONFIG_MANIFEST_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_nv_config_manifest)
endif()

if(CONFIG_BB_NV_FACTORY_RESET_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_nv_factory_reset_routes)
endif()

# bb_nv has no platform/host/bb_nv/ — its core impl + routes are portable
# (guarded by #ifdef ESP_PLATFORM internally) but filed under platform/espidf/
# only; the host build needs them (and the private headers colocated there).
# bbtool-scaffold-hint: source=platform/espidf/bb_nv/bb_nv.c
# bbtool-scaffold-hint: source=platform/espidf/bb_nv/bb_nv_routes.c
# bbtool-scaffold-hint: source=platform/espidf/bb_nv/bb_nv_delete_routes.c
# bbtool-scaffold-hint: include=platform/espidf/bb_nv
