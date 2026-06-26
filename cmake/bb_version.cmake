# bb_version.cmake — sets PROJECT_VER by delegating to scripts/bb_version.py.
#
# This file no longer reimplements version logic. It calls bb_version.py --emit
# so there is ONE implementation (the Python script) and cmake consumers get the
# same version format as PlatformIO consumers. (TA-462: the previous duplication
# caused /api/info to show the stale old format after PR #560 updated the .py.)
#
# Precedence (enforced by bb_version.py):
#   1. BB_FW_VERSION env var (non-empty)  → used verbatim
#   2. Consumer repo has an exact git tag at HEAD  → use that tag
#   3. dev default: dev-<tm-ref>-<bb-ref>
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

# 1. BB_FW_VERSION env var override (fast path — skip Python invocation).
if(DEFINED ENV{BB_FW_VERSION} AND NOT "$ENV{BB_FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{BB_FW_VERSION}")
    message(STATUS "bb_version: ${PROJECT_VER} (BB_FW_VERSION env override)")
    return()
endif()

# 2+3. Delegate to bb_version.py --emit for all other cases.
# Prefer Python3_EXECUTABLE if the caller already found Python; fall back to
# python3 on PATH.
if(DEFINED Python3_EXECUTABLE)
    set(_BB_PYTHON "${Python3_EXECUTABLE}")
else()
    set(_BB_PYTHON "python3")
endif()

execute_process(
    COMMAND "${_BB_PYTHON}" "${_BB_ROOT}/scripts/bb_version.py"
            --emit --consumer "${_BB_CONSUMER_DIR}"
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT PROJECT_VER)
    set(PROJECT_VER "dev-unknown")
endif()

message(STATUS "bb_version: ${PROJECT_VER}")
