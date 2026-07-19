# bb_timer is a sweep-only dep (the sweep's bb_timer_worker) -- NOT
# conditionally gated behind CONFIG_BB_CACHE_SWEEP_ENABLE despite that being
# the natural-looking fix, because idf_component_register's requirements are
# expanded in an EARLY pass that runs before Kconfig values are available
# (confirmed empirically: a message(STATUS ...) printing
# CONFIG_BB_CACHE_SWEEP_ENABLE here shows an EMPTY string on the early
# requirements-expansion pass and 'y' only on the later full-registration
# pass) -- a Kconfig-conditional REQUIRES/PRIV_REQUIRES silently resolves as
# if the config were unset, every time, regardless of its real value. The
# rule this drives: always PRIV_REQUIRES a Kconfig-gated optional dependency
# unconditionally -- never behind a CONFIG_ guard in CMakeLists.txt. In
# practice this costs nothing extra in a sweep-off
# build: bb_cache's own use of the bb_timer API is entirely inside
# `#if BB_CACHE_SWEEP_ENABLE`, which preprocesses to nothing when the
# Kconfig is off, so --gc-sections drops any bb_timer symbols this component
# would have pulled in (bb_timer is used elsewhere by bb_led_anim/
# bb_button_events/etc -- so it is already present in a typical firmware
# image regardless of this line).
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache/bb_cache_espidf.c"
         "src/bb_cache_evaluate.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_log bb_str bb_timer)
