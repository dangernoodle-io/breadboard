# cmake/bb_generated.cmake — shared guard for `bbtool codegen` output.
#
# Extracted (second hand-rolled instance, per the Consolidation convention
# in CLAUDE.md) from the "set path -> if(NOT EXISTS ...) FATAL_ERROR ->
# include()" idiom that guards every gitignored `bbtool codegen` fragment
# (generated/bb_autowire_components.cmake, generated/bb_app_init.cmake,
# ...): a missing fragment must be a hard configure-time error, never a
# silent skip, so a build never silently links the wrong (or a stale)
# closure.
#
# bb_include_generated_or_fail(<out_var> <path> <context_msg>)
#   - sets <out_var> to <path> in the caller's scope
#   - FATAL_ERRORs with "<path> is missing -- <context_msg>" if <path>
#     does not exist
#   - include()s <path> otherwise
#
# A macro, not a function: the included fragment's variables (e.g.
# BB_AUTOWIRE_COMPONENTS, BB_AUTOWIRE_REQUIRES) must land in the CALLER's
# scope, and a function's own scope would swallow them.
#
# All example call sites (examples/floor/CMakeLists.txt,
# examples/floor/main/CMakeLists.txt, examples/smoke/main/CMakeLists.txt)
# now use this macro.
macro(bb_include_generated_or_fail out_var path context_msg)
    set(${out_var} "${path}")
    if(NOT EXISTS "${${out_var}}")
        message(FATAL_ERROR "${${out_var}} is missing -- ${context_msg}")
    endif()
    include("${${out_var}}")
endmacro()

# ---------------------------------------------------------------------------
# bb_verify_generated_or_fail(<path> <context_msg>)  -- B1-1371, belt-and-
# braces since B1-1403 (see bb_regenerate_wire_or_fail below)
#
# Originally the sole stale-artifact guard for a generated `bb_app_init.c`:
# the file is gitignored and, before B1-1403, was regenerated only by the
# explicit `make smoke-gen*`/`floor-gen` targets, never by CMake/PlatformIO
# itself. A stale copy left on disk from an earlier checkout -- one whose
# `// bbtool:init fn=` call targets have since been renamed or DELETED from
# every header in the tree -- compiled silently into the image otherwise;
# confirmed on hardware (B1-1371) via a generated file that still called
# `bb_health_reserve_routes()` long after that function was deleted
# everywhere.
#
# Delegates to `bbtool.py verify-generated` (scripts/bbtool/commands/
# verify_generated.py): parses every apparent call target in <path> and
# hard-fails configure if any of them has no matching declaration left
# ANYWHERE under components/, platform/, or examples/. This catches "calls
# something that no longer exists"; it does NOT catch a stale file whose
# called functions all still exist but were re-ordered (see that module's
# docstring for the full scope statement) -- closed separately by B1-1403's
# `bb_regenerate_wire_or_fail`, which now unconditionally regenerates
# `bb_app_init.c` at every real-pass configure instead of merely checking
# it. Both call sites (examples/smoke, examples/floor) now call
# `bb_regenerate_wire_or_fail` FIRST and this function SECOND, purely as
# cheap defense-in-depth against a codegen bug or an out-of-band write.
#
# No-op if <path> doesn't exist (the missing-file case is
# bb_include_generated_or_fail's job) or during ESP-IDF's early-expansion
# REQUIRES-only pass (execute_process is not meaningful there -- mirrors the
# CMAKE_BUILD_EARLY_EXPANSION guard on BB_AUTOWIRE_BOARD in
# examples/smoke/main/CMakeLists.txt).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# bb_regenerate_wire_or_fail(<wire_out> <board> <wire_board> <consumer_manifest>
#                             <context_msg>)  -- B1-1403
#
# Makes `bb_app_init.c` a REAL build dependency instead of a staleness check:
# runs `bbtool.py codegen` unconditionally at every real-pass CMake configure
# and (re)writes <wire_out> from the CURRENT `// bbtool:init` marker set,
# rather than trusting whatever copy already happens to sit on disk. Closes
# the gap `bb_verify_generated_or_fail` (B1-1371) deliberately left open: a
# stale file whose call targets have all been RE-ORDERED (not
# renamed/deleted) still passes that symbol-existence check, because nothing
# there re-derives the correct tier order -- only a real re-resolve does,
# which is exactly what this does instead.
#
# <board>/<wire_board> are passed straight through to `bbtool codegen
# --board/--wire-board` (see that command's module docstring for why they
# can diverge -- examples/smoke pins wire generation to a fixed
# `smoke_wire_baseline` board regardless of which board's REQUIRES set is
# being built). <consumer_manifest> may be an empty string (floor has none).
#
# The REQUIRES/components-fragment half of codegen's output
# (`bb_autowire_components.cmake`) is deliberately NOT touched by this
# function -- it's written to a throwaway scratch path and discarded. That
# fragment stays on its existing Make-driven generation path
# (`make smoke-gen-<board>` / `floor-gen`, still required before a FRESH
# checkout's early-expansion CMake pass, which cannot regenerate it itself:
# see main/CMakeLists.txt's `BB_AUTOWIRE_BOARD` guard comment for why
# externally -D'd cache vars like FIRMWARE_BOARD aren't populated there).
# Scoping this function to `bb_app_init.c` only keeps that pre-existing
# REQUIRES-fragment contract completely unchanged -- this ticket is about
# wire staleness, not the fragment's own bootstrap path.
#
# Regeneration only happens on the SECOND (real) CMake pass, mirroring
# `bb_verify_generated_or_fail`'s own `CMAKE_BUILD_EARLY_EXPANSION` guard,
# for the same reason: `bb_app_init.c` is a compiled SRCS entry, never
# examined by the REQUIRES-only early-expansion pass, so there is nothing to
# regenerate for correctly on that pass.
#
# Callers must ALSO register every codegen input (component/platform
# headers, the consumer manifest, the bbtool sources themselves,
# bbtool.toml) via `CMAKE_CONFIGURE_DEPENDS` so ninja re-invokes CMake
# configure (and therefore this function) whenever any of them change --
# this function only makes regeneration UNCONDITIONAL once configure runs;
# it does not by itself make configure re-run on a source-only edit. See the
# example CMakeLists.txt call sites for the actual dependency list.
#
# `bbtool codegen`'s own writer (`_write_if_changed`,
# scripts/bbtool/commands/codegen.py) is content-comparing and only replaces
# <wire_out> on the filesystem when the newly-resolved content actually
# differs, so a configure re-run whose resolved output is unchanged leaves
# <wire_out>'s mtime untouched -- no needless downstream recompile.
#
# CMAKE_CONFIGURE_DEPENDS DUPLICATE-OUTPUT HAZARD (B1-1407, FIXED): the
# original B1-1403 landing had each example's main/CMakeLists.txt register
# EVERY component/platform CMakeLists.txt as an explicit CONFIGURE_DEPENDS
# entry, to catch a REQUIRES/PRIV_REQUIRES-only edit with no header touched
# (see the example CMakeLists.txt's own comment for why headers alone don't
# cover that). That collided with ESP-IDF's OWN implicit tracking: ESP-IDF's
# build system (framework-espidf/CMakeLists.txt's
# `foreach(component_target ${build_component_targets}) ...
# add_subdirectory(${dir} ${name})` loop) already add_subdirectory's every
# component in the real per-board closure, and CMake automatically tracks
# every add_subdirectory'd CMakeLists.txt as an implicit reconfigure
# dependency with no CONFIGURE_DEPENDS needed at all. Registering that SAME
# absolute path a SECOND time, explicitly, from a component's own directory
# scope, made CMake's Ninja generator emit it as an output of the merged
# RERUN_CMAKE build edge TWICE -- a hard "output defined multiple times"
# ninja error that PlatformIO could mask behind an overall [SUCCESS] while
# silently SKIPPING the reconfigure. This reproduced on a genuinely FRESH
# build dir's SECOND configure, not just a `.pio/build` predating this
# function's CONFIGURE_DEPENDS glob.
#
# The fix: each example's CMakeLists.txt now filters its CMakeLists.txt
# candidate list (every CMakeLists.txt under components/ and platform/)
# against `idf_build_get_property(... BUILD_COMPONENTS)` -- ESP-IDF's own
# final, per-board-resolved build-component list (NOT `BB_AUTOWIRE_REQUIRES`,
# a much narrower set -- see the example CMakeLists.txt's own comment for
# why) -- and only registers a candidate explicitly if its owning component
# is NOT in that list, i.e. only for a component ESP-IDF genuinely is NOT
# add_subdirectory-ing for that specific board/build.
#
# This is a live, currently-exercised gap for BOTH examples, not dormant
# scaffolding for a hypothetical future -- verified empirically (B1-1403
# review) by instrumenting a real `pio run` configure of each on a fresh
# build dir. `examples/smoke -e esp32` (board esp32_wroom_32) registers
# exactly two paths: components/display/bb_display_ek79007/CMakeLists.txt
# and components/display/bb_display_st77xx/CMakeLists.txt -- that board's
# EXCLUDE_COMPONENTS (examples/smoke/CMakeLists.txt) keeps them out of
# BUILD_COMPONENTS, so ESP-IDF never add_subdirectory's them and CMake's
# implicit tracking never covers them; this explicit registration is the
# ONLY thing tracking them for reconfigure purposes. (Neither carries a `//
# bbtool:init` marker as of this writing, so the excluded-AND-marker-bearing
# scenario is structurally covered but not yet exercised live.)
# `examples/floor -e esp32` registers far more -- 43 distinct components
# (bb_attrs, bb_collection, bb_fan, bb_fan_emc2101, bb_filter, bb_fmt,
# bb_http_client, bb_i2c, bb_mdns_cache, bb_mem_arena_tls, bb_ota_boot,
# bb_ota_check, bb_ota_hooks, bb_ota_pull, bb_ota_push (both its components/
# and platform/espidf/ directories), bb_pool, bb_power, bb_power_health,
# bb_power_tps546, bb_release_manifest, bb_ring_diag, bb_scalar, bb_sensor,
# bb_sensor_http, bb_serialize_logfmt, bb_tcp_client, bb_udp_client,
# bb_udp_frame, bb_ws_server, bb_button, bb_button_events, bb_button_gpio,
# bb_display, bb_display_ek79007, bb_display_spi_common, bb_display_st77xx,
# bb_led, bb_led_anim, bb_led_apa102, bb_led_gpio, bb_led_pwm,
# bb_led_rgb_pwm, bb_storage_ram, bb_queue_espidf) -- floor's narrow
# BB_AUTOWIRE_COMPONENTS-derived `COMPONENTS` allowlist
# (examples/floor/CMakeLists.txt) keeps every component outside that
# allowlist out of BUILD_COMPONENTS entirely, so this filter is what tracks
# all of them for reconfigure purposes. No local-checkout workaround (like
# removing `.pio/build`) is needed for this hazard any more -- a fresh build
# dir's second and subsequent configures are clean.
# ---------------------------------------------------------------------------

function(bb_regenerate_wire_or_fail wire_out board wire_board consumer_manifest context_msg)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()

    get_filename_component(_bb_gen_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    get_filename_component(_bb_gen_root "${_bb_gen_cmake_dir}/.." ABSOLUTE)

    if(DEFINED Python3_EXECUTABLE)
        set(_bb_regen_py "${Python3_EXECUTABLE}")
    else()
        set(_bb_regen_py "python3")
    endif()

    # Scratch, discarded output for the REQUIRES/components-fragment half of
    # codegen's single resolution -- see the docstring above for why this
    # function never touches the real bb_autowire_components.cmake.
    set(_bb_regen_scratch_components "${CMAKE_BINARY_DIR}/bb_codegen_scratch/bb_autowire_components.cmake")

    set(_bb_regen_cmd
        "${_bb_regen_py}" "${_bb_gen_root}/scripts/bbtool.py" codegen
        --root "${_bb_gen_root}"
        --board "${board}"
        --wire-board "${wire_board}"
        --components-out "${_bb_regen_scratch_components}"
        --wire-out "${wire_out}")
    if(consumer_manifest)
        list(APPEND _bb_regen_cmd --consumer-manifest "${consumer_manifest}")
    endif()

    execute_process(
        COMMAND ${_bb_regen_cmd}
        RESULT_VARIABLE _bb_regen_rc
        OUTPUT_VARIABLE _bb_regen_out
        ERROR_VARIABLE _bb_regen_err
    )
    if(NOT _bb_regen_rc EQUAL 0)
        message(FATAL_ERROR "${wire_out}: ${context_msg}\n${_bb_regen_out}\n${_bb_regen_err}")
    endif()
endfunction()

function(bb_verify_generated_or_fail path context_msg)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    if(NOT EXISTS "${path}")
        return()
    endif()

    get_filename_component(_bb_gen_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    get_filename_component(_bb_gen_root "${_bb_gen_cmake_dir}/.." ABSOLUTE)

    if(DEFINED Python3_EXECUTABLE)
        set(_bb_gen_py "${Python3_EXECUTABLE}")
    else()
        set(_bb_gen_py "python3")
    endif()

    execute_process(
        COMMAND "${_bb_gen_py}" "${_bb_gen_root}/scripts/bbtool.py"
                verify-generated --root "${_bb_gen_root}" --file "${path}"
        RESULT_VARIABLE _bb_gen_rc
        OUTPUT_VARIABLE _bb_gen_out
        ERROR_VARIABLE _bb_gen_err
    )
    if(NOT _bb_gen_rc EQUAL 0)
        message(FATAL_ERROR "${path}: ${context_msg}\n${_bb_gen_out}${_bb_gen_err}")
    endif()
endfunction()
