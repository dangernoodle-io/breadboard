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
# build artifact, never committed); floor_app.c's app_main() stays the sole
# entry point either way -- bb_app_init() is never called, only compiled, by
# this proof.
#
# B1-1403: `bb_regenerate_wire_or_fail` below makes bb_app_init.c generation
# itself a real CMake build step -- it (re)runs `bbtool codegen`
# unconditionally on every real-pass configure, so bb_app_init.c cannot go
# stale by construction once it exists at all.
#
# This does NOT make a bare `pio run -d examples/floor` work on a genuinely
# fresh checkout: the REQUIRES/components-fragment half of codegen's output
# (generated/bb_autowire_components.cmake, bb_include_generated_or_fail
# below) is deliberately untouched by bb_regenerate_wire_or_fail -- see that
# function's docstring -- and stays on its pre-existing Make-driven path
# (`make floor-gen`), still required once before the first build. A plain
# `pio run`/`make floor` AFTER that fragment exists, though, now regenerates
# bb_app_init.c fresh at every configure with no further `make floor-gen`
# needed.
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bb_generated.cmake")
set(BB_FLOOR_GENERATED_APP_INIT "${CMAKE_CURRENT_LIST_DIR}/generated/bb_app_init.c")
bb_regenerate_wire_or_fail(
    "${BB_FLOOR_GENERATED_APP_INIT}"
    "floor"
    "floor"
    ""
    "floor: failed to regenerate generated/bb_app_init.c via bbtool codegen")

# CMAKE_CONFIGURE_DEPENDS (B1-1403): see examples/smoke/main/CMakeLists.txt's
# identical block for the full rationale -- forces a CMake reconfigure (and
# therefore bb_regenerate_wire_or_fail above) whenever a codegen input
# changes, not just when this CMakeLists.txt itself does, INCLUDING a
# component/platform CMakeLists.txt's own REQUIRES/PRIV_REQUIRES (read by
# `boards.derive_component`, entirely independent of that component's
# headers -- see the smoke block's comment for the full trace). Guarded the
# same way, and for the same reason: CONFIGURE_DEPENDS hard-errors in the
# plain script-mode `include()` ESP-IDF's REQUIRES-only early-expansion pass
# uses to evaluate this file.
#
# B1-1407: de-duplicated against `idf_build_get_property(... BUILD_COMPONENTS)`
# the same way as examples/smoke/main/CMakeLists.txt (see that file's
# identical block for the full rationale, including why `BB_AUTOWIRE_REQUIRES`
# alone is NOT the right ground truth) -- floor's top-level CMakeLists.txt
# DOES set a project-level `COMPONENTS` allowlist from `BB_AUTOWIRE_COMPONENTS`
# before `include(project.cmake)` (unlike smoke), so its real, ESP-IDF-built
# component set should already match `BB_AUTOWIRE_REQUIRES` closely -- using
# the same `BUILD_COMPONENTS`-based check here anyway keeps both examples'
# de-duplication logic identical and verifiably correct rather than relying
# on that discovery-config difference staying true.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB_RECURSE _bb_floor_codegen_header_deps CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../../../components/*.h"
        "${CMAKE_CURRENT_LIST_DIR}/../../../platform/*.h")

    # Plain (non-CONFIGURE_DEPENDS) glob: see the identical block in
    # examples/smoke/main/CMakeLists.txt for why CONFIGURE_DEPENDS must NOT
    # be used directly on this glob.
    idf_build_get_property(_bb_floor_real_build_components BUILD_COMPONENTS)
    file(GLOB_RECURSE _bb_floor_codegen_cmakelists_candidates
        "${CMAKE_CURRENT_LIST_DIR}/../../../components/CMakeLists.txt"
        "${CMAKE_CURRENT_LIST_DIR}/../../../platform/CMakeLists.txt")
    set(_bb_floor_codegen_cmakelists_deps "")
    foreach(_bb_floor_cmakelists_cand ${_bb_floor_codegen_cmakelists_candidates})
        get_filename_component(_bb_floor_cand_dir "${_bb_floor_cmakelists_cand}" DIRECTORY)
        get_filename_component(_bb_floor_cand_name "${_bb_floor_cand_dir}" NAME)
        if(NOT _bb_floor_cand_name IN_LIST _bb_floor_real_build_components)
            list(APPEND _bb_floor_codegen_cmakelists_deps "${_bb_floor_cmakelists_cand}")
        endif()
    endforeach()

    file(GLOB_RECURSE _bb_floor_codegen_tool_deps CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/bbtool/*.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_bb_floor_codegen_header_deps}
        ${_bb_floor_codegen_cmakelists_deps}
        ${_bb_floor_codegen_tool_deps}
        "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/bbtool.py"
        "${CMAKE_CURRENT_LIST_DIR}/../../../bbtool.toml")
endif()

# floor_prov_reboot.c / floor_task_stack.c: pure decision guards (B1-809
# review fix; B1-1418 PR-2 review fix), split out purely so
# test/test_host/test_floor_prov_reboot.c / test_floor_task_stack.c can
# #include these translation units directly -- examples aren't part of the
# native scaffold's component graph, so a host test has no other way to
# reach them.
set(_floor_srcs "floor_app.c" "floor_prov_reboot.c" "floor_task_stack.c")
if(EXISTS "${BB_FLOOR_GENERATED_APP_INIT}")
    list(APPEND _floor_srcs "generated/bb_app_init.c")
endif()

# B1-747: REQUIRES is now resolved by `bbtool codegen --board floor` from the
# [capability.floor]/[board.floor] manifest in bbtool.toml (see
# scripts/bbtool/boards.py) instead of the hand-literal list this replaces
# (bb_log bb_meminfo bb_timer) -- floor has a single board/env, so unlike
# smoke there's no board-guard comparison to make (no FIRMWARE_BOARD is
# defined for this build); a missing fragment is still a hard error.
# (cmake/bb_generated.cmake already include()d above.)
bb_include_generated_or_fail(BB_FLOOR_GENERATED_FRAGMENT
    "${CMAKE_CURRENT_LIST_DIR}/generated/bb_autowire_components.cmake"
    "floor: run `make floor-gen` (see the Makefile) before building.")

# B1-1371 + B1-1403: belt-and-braces symbol-existence check, kept as
# defense-in-depth NOW THAT bb_regenerate_wire_or_fail above already
# unconditionally regenerates bb_app_init.c from the current marker set --
# this still catches a codegen bug in that regeneration itself, a hand-edit
# of the generated file after configure, or any future path that writes it
# out-of-band. See cmake/bb_generated.cmake's bb_verify_generated_or_fail
# docstring for exact scope.
bb_verify_generated_or_fail("${BB_FLOOR_GENERATED_APP_INIT}"
    "floor: STALE generated/bb_app_init.c after regeneration -- this should be unreachable; report a bug.")

idf_component_register(
    SRCS ${_floor_srcs}
    INCLUDE_DIRS "."
    REQUIRES ${BB_AUTOWIRE_REQUIRES})
