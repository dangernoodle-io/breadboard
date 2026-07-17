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
