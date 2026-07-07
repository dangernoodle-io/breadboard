cmake_minimum_required(VERSION 3.16.0)

# FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): the minimal
# bootable base firmware -- bb_log + while(1) print, HAND-WIRED (a la
# carte, no bb_init walker). Every layered-up bb example should be
# measurably larger than this floor.
#
# Same COMPONENTS-allowlist lever as examples/autowire/CMakeLists.txt: a
# project-level `set(COMPONENTS ...)` gates which components ESP-IDF even
# discovers/configures under EXTRA_COMPONENT_DIRS, expanded recursively via
# each discovered component's own REQUIRES/PRIV_REQUIRES.
#
# Usage (never `pio ci`; regenerate the fragment, clean, rebuild, measure):
#   python3 ../../scripts/bbtool.py autowire --root ../.. \
#       --components "bb_log,bb_meminfo" \
#       --out main/generated/bb_autowire_components.cmake
#   pio run -d . -e esp32
#   python3 ../../scripts/bbtool.py size --build-dir .pio/build/esp32

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components"
)

# Same P4-only-target workaround as examples/autowire/CMakeLists.txt: the
# component-manager's manifest-solve pass walks every idf_component.yml under
# EXTRA_COMPONENT_DIRS before COMPONENTS is applied, so bb_display_ek79007's
# esp32p4-only pin must be excluded even though this example never selects it.
set(EXCLUDE_COMPONENTS bb_display_ek79007)

set(BB_AUTOWIRE_OUT "${CMAKE_CURRENT_SOURCE_DIR}/main/generated/bb_autowire_components.cmake")

if(NOT EXISTS "${BB_AUTOWIRE_OUT}")
    message(FATAL_ERROR
        "Missing ${BB_AUTOWIRE_OUT} -- run:\n"
        "  python3 ../../scripts/bbtool.py autowire --root ../.. "
        "--components \"bb_log,bb_meminfo\" --out main/generated/bb_autowire_components.cmake\n"
        "first (see the usage comment at the top of this file).")
endif()

# Sets BB_AUTOWIRE_REQUIRES and BB_AUTOWIRE_COMPONENTS. Included BEFORE
# project.cmake so `set(COMPONENTS ...)` below is honored.
include("${BB_AUTOWIRE_OUT}")

# THE FLOOR: an explicit project-level COMPONENTS allowlist restricted to the
# true minimal closure. `__pio_env` is PlatformIO's own auto-generated dummy
# component, required regardless of this allowlist.
set(COMPONENTS __pio_env ${BB_AUTOWIRE_COMPONENTS})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(floor)
