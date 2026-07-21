# cmake/bb_component_dirs.cmake — nested-layout EXTRA_COMPONENT_DIRS helper.
#
# ESP-IDF's component manager scans each EXTRA_COMPONENT_DIRS entry exactly
# ONE level deep: every immediate child directory of an entry that contains
# a CMakeLists.txt becomes a discovered component. A
# components/<group>/<name>/CMakeLists.txt nested two levels under a plain
# "components" entry is invisible to it (only components/<group>/ itself
# would register, and since IT has no CMakeLists.txt of its own, it's
# simply skipped — <name> never surfaces).
#
# bb_component_dirs() computes the full EXTRA_COMPONENT_DIRS entry list a
# consumer needs to see every component regardless of grouping: the
# components/ root itself (covers today's flat components/<name>/ layout)
# plus one extra entry per direct components/<group>/ subdirectory that has
# NO CMakeLists.txt of its own (a group/intermediate directory, never a
# leaf component itself — mirrors scripts/bbtool/discovery.py's leaf rule)
# — one level deeper under THAT entry reveals its real component children.
#
# Generated, not hand-maintained: a future canary/mass move to
# components/<group>/<name>/ needs zero edits here or in any example's
# CMakeLists.txt — the glob below picks up the new group directory
# automatically at configure time.
#
# No-op on a flat tree with no group directories: the glob finds zero such
# subdirectories, so the output list is just [components/] unchanged.
#
# Two known, currently-acceptable limits vs. the Python discovery SSOT:
#   (a) ONE level of grouping only — unlike scripts/bbtool/discovery.py's
#       arbitrary-depth leaf rule, this only globs one level under
#       components/. A group-of-groups (components/<g1>/<g2>/<name>/) would
#       be indexed fine by the Python tooling but stays invisible here —
#       ESP-IDF just never discovers <name>'s CMakeLists.txt, which fails
#       LOUD at build time (a missing/unbuilt component), never silently.
#       Still an inconsistently-scoped limit worth knowing before ever
#       nesting two levels deep.
#   (b) file(GLOB) is CONFIGURE-TIME only — a new group directory added
#       after the last `cmake`/`pio run` configure needs a re-configure
#       (e.g. `pio run -t clean` before the next build, or just fresh
#       `.pio/build/*` state) to be picked up; an incremental build reusing
#       a stale CMakeCache won't see it.
#
# Usage (before project()):
#   include("<breadboard_root>/cmake/bb_component_dirs.cmake")
#   bb_component_dirs("<path>/components" MY_EXTRA_DIRS)
#   list(APPEND EXTRA_COMPONENT_DIRS ${MY_EXTRA_DIRS})
function(bb_component_dirs components_root out_var)
    set(_bb_dirs "${components_root}")
    if(IS_DIRECTORY "${components_root}")
        file(GLOB _bb_children RELATIVE "${components_root}" "${components_root}/*")
        foreach(_bb_child ${_bb_children})
            set(_bb_child_dir "${components_root}/${_bb_child}")
            if(IS_DIRECTORY "${_bb_child_dir}" AND NOT EXISTS "${_bb_child_dir}/CMakeLists.txt")
                list(APPEND _bb_dirs "${_bb_child_dir}")
            endif()
        endforeach()
    endif()
    set(${out_var} "${_bb_dirs}" PARENT_SCOPE)
endfunction()
