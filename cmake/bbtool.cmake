# cmake/bbtool.cmake — breadboard tooling bridge.
#
# On include() (before project()), sets PROJECT_VER by delegating to
# scripts/bbtool.py version --emit so there is ONE implementation.
#
# Commit C delivers: version-on-include only.
# Commit D adds: bb_embed_assets(), bb_embed_site(), bb_lint() target.
#
# Usage (in consumer CMakeLists.txt, before project()):
#   include("<breadboard_root>/cmake/bbtool.cmake")
#   # PROJECT_VER is now set.

cmake_minimum_required(VERSION 3.16)

get_filename_component(_BB_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_BB_ROOT "${_BB_CMAKE_DIR}/.." ABSOLUTE)

set(_BB_CONSUMER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# 1. BB_FW_VERSION env var override (fast path).
if(DEFINED ENV{BB_FW_VERSION} AND NOT "$ENV{BB_FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{BB_FW_VERSION}")
    message(STATUS "bb_version: ${PROJECT_VER} (BB_FW_VERSION env override)")
    return()
endif()

# 2+3. Delegate to bbtool.py version --emit.
if(DEFINED Python3_EXECUTABLE)
    set(_BB_PYTHON "${Python3_EXECUTABLE}")
else()
    set(_BB_PYTHON "python3")
endif()

execute_process(
    COMMAND "${_BB_PYTHON}" "${_BB_ROOT}/scripts/bbtool.py"
            version --emit --consumer "${_BB_CONSUMER_DIR}"
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT PROJECT_VER)
    set(PROJECT_VER "dev-unknown")
endif()

message(STATUS "bb_version: ${PROJECT_VER}")
