idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http_server/bb_http.c"
         "src/http_utils.c"
         "src/bb_http_status.c"
         "src/bb_http_query.c"
         "src/bb_http_body.c"
         "src/route_registry.c"
         "src/bb_http_json_obj.c"
         "src/bb_dispatch_api.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http
    PRIV_REQUIRES bb_nv bb_log bb_json esp_http_server esp_event bb_str bb_init
)

# bb_http_server self-registers its HTTP-autostart (PRE_HTTP-tier) and
# route-registry audit (REGULAR-tier) hooks via
# BB_INIT_REGISTER_PRE_HTTP_N/BB_INIT_REGISTER_N (see bb_http.c) -- force-keep
# both constructor-only entry points under PlatformIO's espidf builder, which
# strips ESP-IDF's WHOLE_ARCHIVE flag.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_HTTP_AUTOSTART)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_http_autostart)
endif()
bb_init_force_register(${COMPONENT_LIB} bb_http_route_audit)

# LWIP_NETCONN_SEM_PER_THREAD is a lwip C-macro, NOT an ESP-IDF Kconfig
# symbol — so Kconfig's `select LWIP_NETCONN_SEM_PER_THREAD` is a silent
# no-op (verified: CONFIG_BB_HTTP_LWIP_SEM_PER_THREAD=1 ships but
# CONFIG_LWIP_NETCONN_SEM_PER_THREAD never gets defined). Inject the
# define via the build-wide compile-options property when the BB knob is
# on; lwip's source picks it up because lwipopts uses the standard
# `#if !defined ... #define ... 0 #endif` shape.
if(CONFIG_BB_HTTP_LWIP_SEM_PER_THREAD)
    idf_build_set_property(COMPILE_OPTIONS "-DLWIP_NETCONN_SEM_PER_THREAD=1" APPEND)
endif()
