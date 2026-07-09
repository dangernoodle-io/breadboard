# bb_init and bb_timer are sweep-only deps (the PRE_HTTP bb_init hook that
# starts the sweep's bb_timer_worker) -- NOT conditionally gated behind
# CONFIG_BB_CACHE_SWEEP_ENABLE despite that being the natural-looking fix,
# because idf_component_register's requirements are expanded in an EARLY
# pass that runs before Kconfig values are available (confirmed empirically:
# a message(STATUS ...) printing CONFIG_BB_CACHE_SWEEP_ENABLE here shows an
# EMPTY string on the early requirements-expansion pass and 'y' only on the
# later full-registration pass) -- a Kconfig-conditional REQUIRES/PRIV_REQUIRES
# silently resolves as if the config were unset, every time, regardless of
# its real value. This matches the established codebase convention seen in
# bb_pub_health's and bb_alert's CMakeLists.txt: always PRIV_REQUIRES the
# optional dependency unconditionally, and gate only the runtime
# registration call (below) behind the Kconfig -- which DOES see the real
# Kconfig value, because it runs after idf_component_register in the same
# (later) pass. In practice this costs nothing extra in a sweep-off build:
# bb_cache's own use of bb_init/bb_timer APIs is entirely inside
# `#if BB_CACHE_SWEEP_ENABLE`, which preprocesses to nothing when the
# Kconfig is off, so --gc-sections drops any bb_init/bb_timer symbols this
# component would have pulled in (both components are near-universally
# required elsewhere already -- bb_init by virtually every self-registering
# component, bb_timer by bb_pub/bb_led_anim/bb_button_events/etc -- so they
# are already present in a typical firmware image regardless of this line).
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache/bb_cache_espidf.c"
         "src/bb_cache_evaluate.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json bb_event
    PRIV_REQUIRES bb_log bb_str bb_init bb_timer)

# bb_init PRE_HTTP registration for the age-out sweep backstop (B1-592
# lifecycle follow-up) -- espidf platform layer only. bb_cache_espidf.c is
# shared verbatim with the host build (platform/host/bb_cache/
# bb_cache_host.c #includes it directly, bypassing this CMakeLists.txt
# entirely), so this force-keep is scoped to the ESP-IDF build alone; the
# portable component and host platform stay on pure lazy ensure_init.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
if(CONFIG_BB_CACHE_SWEEP_ENABLE)
    bb_init_force_register_pre_http(${COMPONENT_LIB} bb_cache_evict)
endif()
