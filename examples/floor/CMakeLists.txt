cmake_minimum_required(VERSION 3.16.0)

# FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): the minimal
# bootable base firmware -- bb_log + while(1) print, HAND-WIRED (a la
# carte, no bb_init walker). Every layered-up bb example should be
# measurably larger than this floor.
#
# `set(COMPONENTS ...)` below is a project-level ESP-IDF allowlist lever: it
# gates which components ESP-IDF even discovers/configures under
# EXTRA_COMPONENT_DIRS. Unlike the (now-removed) `bbtool autowire`-generated
# fragment this used to include, the list is STATIC -- floor's real link
# closure (main's `REQUIRES bb_log bb_meminfo bb_timer` in
# main/CMakeLists.txt, expanded transitively through each component's own
# REQUIRES/PRIV_REQUIRES) doesn't change without a source edit, so there is
# no generated artifact to regenerate or go stale. Verify against
# main/CMakeLists.txt's REQUIRES + each listed component's own CMakeLists.txt
# REQUIRES/PRIV_REQUIRES before editing this list.
#
# Usage (never `pio ci`):
#   pio run -d . -e esp32
#   python3 ../../scripts/bbtool.py size --build-dir .pio/build/esp32
#
# The codegen path (`bbtool codegen`) mirroring this same component set is
# proven separately via `make floor-codegen` (see repo Makefile) -- it does
# not touch this static allowlist, and floor's primary build stays
# hand-wired.

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components"
)

# Same P4-only-target workaround as examples/smoke/CMakeLists.txt: the
# component-manager's manifest-solve pass walks every idf_component.yml under
# EXTRA_COMPONENT_DIRS before COMPONENTS is applied, so bb_display_ek79007's
# esp32p4-only pin must be excluded even though this example never selects it.
set(EXCLUDE_COMPONENTS bb_display_ek79007)

# THE FLOOR: an explicit project-level COMPONENTS allowlist restricted to the
# true minimal closure -- main's own seeds (bb_log, bb_meminfo, bb_timer)
# plus their real transitive REQUIRES/PRIV_REQUIRES closure (bb_core,
# bb_str, bb_registry, bb_task, bb_mem_arena, bb_wdt). `__pio_env` is
# PlatformIO's own auto-generated dummy component, required regardless of
# this allowlist.
set(COMPONENTS __pio_env main
    bb_log bb_meminfo bb_timer
    bb_core bb_str bb_registry bb_task bb_mem_arena bb_wdt
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(floor)
