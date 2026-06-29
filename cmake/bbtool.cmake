# cmake/bbtool.cmake — breadboard tooling bridge.
#
# On include() (before project()), sets PROJECT_VER by delegating to
# scripts/bbtool.py version --emit so there is ONE implementation.
#
# Delivers: version-on-include, bb_embed_assets(), bb_embed_site(), bb_lint().
#
# Usage (in consumer CMakeLists.txt, before project()):
#   include("<breadboard_root>/cmake/bbtool.cmake")
#   # PROJECT_VER is now set.
#   bb_lint()  # opt-in: cmake --build build --target bb_lint

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

# ---------------------------------------------------------------------------
# bb_embed_assets(OUT_SRCS <var> ASSETS <file>:<symbol> [...])
#
# Generates gzipped C byte-array source files for each asset by invoking
# bbtool.py embed at CMake CONFIGURE time (not build time). Appends generated
# .c paths to <OUT_SRCS> in the caller's scope.
#
# Rationale: build-time add_custom_command hits PlatformIO's scons path-
# doubling bug on Linux CI when OUTPUT is under CMAKE_CURRENT_BINARY_DIR.
# Generating at configure time sidesteps that — files exist before scons
# scans. CONFIGURE_DEPENDS on each input forces reconfigure on file change.
#
# Input paths resolve relative to CMAKE_CURRENT_LIST_DIR (caller's dir).
# ---------------------------------------------------------------------------

function(bb_embed_assets)
    cmake_parse_arguments(ARG "" "OUT_SRCS" "ASSETS" ${ARGN})
    get_filename_component(_bb_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    get_filename_component(_bb_root "${_bb_cmake_dir}/.." ABSOLUTE)
    set(_script "${_bb_root}/scripts/bbtool.py")
    set(_gen "")

    # PlatformIO's ESP-IDF integration runs an inspection configure pass before
    # the real component build. In that pass, this component's binary dir is
    # not yet resolved and equals the project source dir — generating there
    # leaks .c artifacts into the example/firmware source tree. Skip it; the
    # real configure pass that follows uses a proper per-component binary dir.
    if(CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        return()
    endif()

    if(DEFINED Python3_EXECUTABLE)
        set(_py "${Python3_EXECUTABLE}")
    else()
        set(_py "python3")
    endif()

    foreach(_pair ${ARG_ASSETS})
        string(REPLACE ":" ";" _parts "${_pair}")
        list(GET _parts 0 _file)
        list(GET _parts 1 _symbol)
        set(_in "${CMAKE_CURRENT_LIST_DIR}/${_file}")
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_symbol}.c")

        execute_process(
            COMMAND "${_py}" "${_script}" embed "${_in}" "${_out}" "${_symbol}"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr
        )
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "bb_embed_assets: failed to generate ${_symbol}: ${_stderr}")
        endif()

        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_in}")
        list(APPEND _gen "${_out}")
    endforeach()

    set(${ARG_OUT_SRCS} ${_gen} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# bb_embed_site(OUT_SRCS <var> TABLE <sym> DIST_DIR <dir> [URL_PREFIX <p>])
#
# Walks <dir> at CMake CONFIGURE time, calls bbtool.py gen-site to produce
# one gzipped blob .c per file plus a <sym>_table.c with the bb_http_asset_t[]
# table and lazy accessor. Appends all generated .c paths to <OUT_SRCS> in
# caller scope.
#
# Reconfigures automatically when any file under DIST_DIR changes
# (CONFIGURE_DEPENDS via file(GLOB_RECURSE)).
#
# DIST_DIR resolves relative to CMAKE_CURRENT_LIST_DIR (caller's directory).
#
# Generated C API:
#   bb_http_asset_t <sym>[];                      // mutable table (len filled at runtime)
#   const bb_http_asset_t *<sym>_get(size_t *n);  // idempotent lazy accessor
# ---------------------------------------------------------------------------

function(bb_embed_site)
    cmake_parse_arguments(ARG "" "OUT_SRCS;TABLE;DIST_DIR;URL_PREFIX" "" ${ARGN})

    # Inspection-pass guard: skip when binary dir is not yet resolved.
    if(CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        return()
    endif()

    if(NOT ARG_TABLE)
        message(FATAL_ERROR "bb_embed_site: TABLE is required")
    endif()
    if(NOT ARG_DIST_DIR)
        message(FATAL_ERROR "bb_embed_site: DIST_DIR is required")
    endif()

    get_filename_component(_bb_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    get_filename_component(_bb_root "${_bb_cmake_dir}/.." ABSOLUTE)
    set(_script "${_bb_root}/scripts/bbtool.py")

    if(DEFINED Python3_EXECUTABLE)
        set(_py "${Python3_EXECUTABLE}")
    else()
        set(_py "python3")
    endif()

    # Resolve DIST_DIR relative to caller's list dir
    if(NOT IS_ABSOLUTE "${ARG_DIST_DIR}")
        set(_dist_abs "${CMAKE_CURRENT_LIST_DIR}/${ARG_DIST_DIR}")
    else()
        set(_dist_abs "${ARG_DIST_DIR}")
    endif()

    # Build optional --url-prefix argument
    set(_prefix_arg "")
    if(ARG_URL_PREFIX)
        set(_prefix_arg "--url-prefix" "${ARG_URL_PREFIX}")
    endif()

    execute_process(
        COMMAND "${_py}" "${_script}"
            gen-site
            "${_dist_abs}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "${ARG_TABLE}"
            ${_prefix_arg}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE  _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "bb_embed_site: gen-site failed for ${ARG_TABLE}: ${_stderr}")
    endif()

    # Split stdout (one absolute .c path per line) into a CMake list
    string(REPLACE "\n" ";" _gen "${_stdout}")

    # Register dist dir contents as configure-time dependencies
    file(GLOB_RECURSE _site_inputs CONFIGURE_DEPENDS "${_dist_abs}/*")

    set(${ARG_OUT_SRCS} ${_gen} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# bb_lint([ROOT <dir>] [PROFILE <consumer|library>] [TARGET <name>])
#
# Creates an opt-in custom target (NOT in ALL) that runs breadboard's
# conventions linter via bbtool.py lint. Invoke with:
#   cmake --build <build_dir> --target <TARGET>
#
# Defaults: ROOT=${CMAKE_SOURCE_DIR}, PROFILE=consumer, TARGET=bb_lint.
# Guard: if the target already exists this call is a no-op, so multiple
# include()s or nested projects won't double-define the target.
# ---------------------------------------------------------------------------

function(bb_lint)
    cmake_parse_arguments(ARG "" "ROOT;PROFILE;TARGET" "" ${ARGN})

    if(NOT ARG_ROOT)
        set(ARG_ROOT "${CMAKE_SOURCE_DIR}")
    endif()
    if(NOT ARG_PROFILE)
        set(ARG_PROFILE "consumer")
    endif()
    if(NOT ARG_TARGET)
        set(ARG_TARGET "bb_lint")
    endif()

    if(TARGET ${ARG_TARGET})
        return()
    endif()

    get_filename_component(_bb_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    get_filename_component(_bb_root "${_bb_cmake_dir}/.." ABSOLUTE)
    set(_script "${_bb_root}/scripts/bbtool.py")

    if(DEFINED Python3_EXECUTABLE)
        set(_py "${Python3_EXECUTABLE}")
    else()
        set(_py "python3")
    endif()

    add_custom_target(${ARG_TARGET}
        COMMAND "${_py}" "${_script}" lint --root "${ARG_ROOT}" --profile "${ARG_PROFILE}"
        COMMENT "bb_lint: checking ${ARG_ROOT} (profile=${ARG_PROFILE})"
        USES_TERMINAL
    )
endfunction()
