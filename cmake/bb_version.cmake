# bb_version.cmake — optional CMake helper that sets PROJECT_VER from the same
# precedence logic used by scripts/bb_version.py.
#
# Precedence:
#   1. BB_FW_VERSION env var (non-empty)  → used verbatim
#   2. Consumer repo has an exact git tag at HEAD  → use that tag
#   3. dev default: dev-g<consumer_short_sha>[-dirty]-bb-g<bb_short_sha>[-dirty]
#
# Usage (in consumer CMakeLists.txt, before project()):
#   include("<breadboard_root>/cmake/bb_version.cmake")
#   # PROJECT_VER is now set; IDF will embed it in esp_app_desc.version.
#
# This is optional — the primary mechanism is scripts/bb_version.py +
# bb_system_get_version().  Include this when you also want esp_app_desc.version
# (visible via `idf.py size`, OTA validator, and /api/info "idf_version") to
# match bb_system_get_version().

cmake_minimum_required(VERSION 3.16)

# Locate the breadboard repo root relative to this file (works via symlink).
get_filename_component(_BB_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_BB_ROOT "${_BB_CMAKE_DIR}/.." ABSOLUTE)

# Consumer repo root = the directory that includes this file.
set(_BB_CONSUMER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# 1. BB_FW_VERSION env var override.
if(DEFINED ENV{BB_FW_VERSION} AND NOT "$ENV{BB_FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{BB_FW_VERSION}")
    message(STATUS "bb_version: ${PROJECT_VER} (BB_FW_VERSION env override)")
    return()
endif()

# 2. Exact tag at consumer HEAD.
execute_process(
    COMMAND git describe --tags --exact-match HEAD
    WORKING_DIRECTORY "${_BB_CONSUMER_DIR}"
    OUTPUT_VARIABLE _BB_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(_BB_TAG)
    set(PROJECT_VER "${_BB_TAG}")
    message(STATUS "bb_version: ${PROJECT_VER} (exact tag)")
    return()
endif()

# 3. Dev default.
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${_BB_CONSUMER_DIR}"
    OUTPUT_VARIABLE _BB_CONSUMER_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY "${_BB_CONSUMER_DIR}"
    OUTPUT_VARIABLE _BB_CONSUMER_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${_BB_ROOT}"
    OUTPUT_VARIABLE _BB_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY "${_BB_ROOT}"
    OUTPUT_VARIABLE _BB_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT _BB_CONSUMER_SHA)
    set(_BB_CONSUMER_SHA "unknown")
endif()
if(NOT _BB_SHA)
    set(_BB_SHA "unknown")
endif()

set(_BB_CONSUMER_PART "g${_BB_CONSUMER_SHA}")
if(_BB_CONSUMER_DIRTY)
    string(APPEND _BB_CONSUMER_PART "-dirty")
endif()

set(_BB_BB_PART "g${_BB_SHA}")
if(_BB_DIRTY)
    string(APPEND _BB_BB_PART "-dirty")
endif()

set(PROJECT_VER "dev-${_BB_CONSUMER_PART}-bb-${_BB_BB_PART}")
message(STATUS "bb_version: ${PROJECT_VER}")
