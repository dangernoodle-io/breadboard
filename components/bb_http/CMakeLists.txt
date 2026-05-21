idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http/bb_http.c"
         "src/http_utils.c"
         "src/route_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_nv bb_log bb_json esp_http_server
)

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
