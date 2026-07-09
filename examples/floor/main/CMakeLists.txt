# FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): HAND-WIRE mode --
# floor_app.c calls bb_log_stream_init()/bb_log_config_init() directly and no
# longer touches the bb_init walker (bb_init_init_early()), so main does NOT
# REQUIRES bb_init itself. bb_log no longer PRIV_REQUIRES bb_init either
# (self-registration machinery -- BB_INIT_REGISTER_EARLY, the -u force-keeps,
# CONFIG_BB_LOG_*_AUTOREGISTER -- was removed from bb_log/bb_log_config), so
# bb_init is fully out of the floor's link closure.
#
# main REQUIRES only the composed seeds -- bb_log's, bb_meminfo's, and
# bb_timer's OWN CMakeLists.txt are evaluated for real by ESP-IDF's actual
# cmake config step (real Kconfig values from sdkconfig), so the true,
# Kconfig-pruned transitive closure is what actually gets linked. The
# top-level CMakeLists.txt's `set(COMPONENTS ...)` is a static allowlist
# mirroring this same closure for discovery purposes only.
#
# `make floor-codegen` (see repo Makefile) regenerates
# generated/bb_app_init.c from this same component set's `// bbtool:init`
# markers via `bbtool codegen`, then rebuilds floor so it gets compiled
# against the real bb_log.h prototypes -- proving the codegen path builds
# end-to-end. That file is gitignored (decision #725, generated code is a
# build artifact, never committed) and absent by default, so this
# conditional is a no-op for floor's normal handwired build; floor_app.c's
# app_main() stays the sole entry point either way -- bb_app_init() is never
# called, only compiled, by this proof.
set(BB_FLOOR_GENERATED_APP_INIT "${CMAKE_CURRENT_SOURCE_DIR}/generated/bb_app_init.c")
set(_floor_srcs "floor_app.c")
if(EXISTS "${BB_FLOOR_GENERATED_APP_INIT}")
    list(APPEND _floor_srcs "generated/bb_app_init.c")
endif()

idf_component_register(
    SRCS ${_floor_srcs}
    INCLUDE_DIRS "."
    REQUIRES bb_log bb_meminfo bb_timer)
