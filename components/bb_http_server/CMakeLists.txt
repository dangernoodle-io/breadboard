idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http_server/bb_http.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http_server/bb_http_section_dispatch.c"
         "src/http_utils.c"
         "src/bb_http_core_steer.c"
         "src/bb_http_status.c"
         "src/bb_http_query.c"
         "src/bb_http_body.c"
         "src/route_registry.c"
         "src/bb_route_match.c"
         "src/bb_http_prov_gate.c"
         "src/bb_http_json_obj.c"
         "src/bb_http_serialize_stream.c"
         "src/bb_http_serialize_error.c"
         "src/bb_http_section.c"
         "src/bb_http_section_status.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_http bb_serialize_json
    PRIV_REQUIRES bb_log esp_http_server esp_event bb_str bb_num bb_wdt
)

# bb_wdt (B1-1364 PR5): the espidf shell (platform/espidf/bb_http_server/
# bb_http.c) calls bb_wdt_claimed_core_mask()/bb_wdt_steer_core() to steer the
# httpd worker task off a core another component has exclusively claimed --
# see src/bb_http_core_steer.c. Not new transitive weight: this component
# already PRIV_REQUIRES bb_log, which PRIV_REQUIRES bb_task, which itself
# PRIV_REQUIRES bb_wdt -- this just makes a genuine direct call explicit
# rather than leaning on that transitive path. No cycle: bb_wdt REQUIRES only
# bb_core and PRIV_REQUIRES esp_system/freertos -- nothing back to
# bb_http_server, directly or transitively.

# bb_nv dropped from PRIV_REQUIRES (B1-963): bb_http.c's #include "bb_nv.h"
# was dead (zero bb_nv_* symbol usage anywhere in this component) and, left
# in place, would have formed a REQUIRES cycle once bb_settings (which bb_nv
# itself PRIV_REQUIRES) gained a PRIV_REQUIRES edge for the relocated
# bb_settings_creds_boot_manifest_init, back when it forwarded to the
# now fully-deleted manifest-registration component (which itself REQUIRES
# bb_http_server publicly) -- bb_settings -> [that component] -> bb_http_server
# -> bb_nv -> bb_settings.

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
