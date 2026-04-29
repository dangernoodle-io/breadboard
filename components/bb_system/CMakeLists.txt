set(_sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system.c")
set(_sys_priv_reqs bb_log esp_app_format esp_timer)

if(CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER)
    list(APPEND _sys_srcs "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_system/bb_system_routes.c")
    list(APPEND _sys_priv_reqs bb_http bb_registry)
endif()

idf_component_register(
    SRCS ${_sys_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_nv
    PRIV_REQUIRES ${_sys_priv_reqs}
)

if(CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
    bb_registry_force_register(${COMPONENT_LIB} bb_system_routes)
endif()
