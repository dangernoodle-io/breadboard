idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http_server/bb_http.c"
         "src/http_utils.c"
         "src/bb_http_status.c"
         "src/bb_http_query.c"
         "src/bb_http_body.c"
         "src/route_registry.c"
         "src/bb_http_json_obj.c"
         "src/bb_http_serialize_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_serialize_json
    PRIV_REQUIRES bb_log bb_json esp_http_server esp_event bb_str
)

# bb_nv dropped from PRIV_REQUIRES (B1-963): bb_http.c's #include "bb_nv.h"
# was dead (zero bb_nv_* symbol usage anywhere in this component) and, left
# in place, would form a REQUIRES cycle once bb_settings (which bb_nv itself
# PRIV_REQUIRES) gained a PRIV_REQUIRES bb_manifest edge for the relocated
# bb_settings_creds_boot_manifest_init (bb_manifest REQUIRES bb_http_server
# publicly) -- bb_settings -> bb_manifest -> bb_http_server -> bb_nv ->
# bb_settings.

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
