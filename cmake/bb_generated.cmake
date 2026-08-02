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
# bb_verify_generated_or_fail(<path> <context_msg>)  -- B1-1371
#
# Stale-artifact guard for a generated `bb_app_init.c`, closing the
# staleness case `bb_include_generated_or_fail`/`BB_AUTOWIRE_BOARD` do not:
# `bb_app_init.c` is gitignored and regenerated only by the explicit `make
# smoke-gen*`/`floor-gen` targets, never by CMake/PlatformIO itself (see
# examples/floor/main/CMakeLists.txt's `floor` target, which has no
# `floor-gen` prerequisite at all). A stale copy left on disk from an
# earlier checkout -- one whose `// bbtool:init fn=` call targets have since
# been renamed or DELETED from every header in the tree -- compiles
# silently into the image otherwise; confirmed on hardware (B1-1371) via a
# generated file that still called `bb_health_reserve_routes()` long after
# that function was deleted everywhere.
#
# Delegates to `bbtool.py verify-generated` (scripts/bbtool/commands/
# verify_generated.py): parses every apparent call target in <path> and
# hard-fails configure if any of them has no matching declaration left
# ANYWHERE under components/, platform/, or examples/. This catches "calls
# something that no longer exists"; it does NOT catch a stale file whose
# called functions all still exist but were re-ordered (see that module's
# docstring for the full scope statement) -- a full pipeline re-resolve at
# every configure would close that gap too, but that restructures how
# codegen is invoked and is out of scope here.
#
# No-op if <path> doesn't exist (the missing-file case is
# bb_include_generated_or_fail's job) or during ESP-IDF's early-expansion
# REQUIRES-only pass (execute_process is not meaningful there -- mirrors the
# CMAKE_BUILD_EARLY_EXPANSION guard on BB_AUTOWIRE_BOARD in
# examples/smoke/main/CMakeLists.txt).
# ---------------------------------------------------------------------------

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
