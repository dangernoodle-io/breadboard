set(_sys_srcs
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_reboot_parse.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_system_boot_banner_format.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system.c")
set(_sys_priv_reqs bb_log esp_app_format bb_http bb_http_server bb_init esp_driver_tsens esp_system bb_ntp bb_json bb_str)

if(CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_routes.c")
endif()

if(CONFIG_BB_SYSTEM_BOOT_BANNER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_boot_banner.c")
endif()

idf_component_register(
    SRCS ${_sys_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_nv ${_sys_priv_reqs}
)

if(CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_system_routes)
    bb_init_force_register(${COMPONENT_LIB} bb_system_routes)
endif()

if(CONFIG_BB_SYSTEM_BOOT_BANNER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register_early(${COMPONENT_LIB} bb_system_boot_banner)
endif()
