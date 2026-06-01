# bb_embed_assets(OUT_SRCS <var> ASSETS <file>:<symbol> [...])
#
# Generates gzipped C byte-array source files for each asset by invoking
# scripts/embed_html.py at CMake CONFIGURE time (not build time). Appends
# generated .c paths to <OUT_SRCS> in the caller's scope.
#
# Rationale: build-time add_custom_command hits PlatformIO's scons path-
# doubling bug on Linux CI when OUTPUT is under CMAKE_CURRENT_BINARY_DIR.
# Generating at configure time sidesteps that — files exist before scons
# scans. CONFIGURE_DEPENDS on each input forces reconfigure on HTML change.
#
# Input paths resolve relative to CMAKE_CURRENT_LIST_DIR (caller's dir).

function(bb_embed_assets)
    cmake_parse_arguments(ARG "" "OUT_SRCS" "ASSETS" ${ARGN})
    get_filename_component(_bb_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
    set(_script "${_bb_cmake_dir}/../scripts/embed_html.py")
    set(_gen "")

    # PlatformIO's ESP-IDF integration runs an inspection configure pass before
    # the real component build. In that pass, this component's binary dir is
    # not yet resolved and equals the project source dir — generating there
    # leaks .c artifacts into the example/firmware source tree. Skip it; the
    # real configure pass that follows uses a proper per-component binary dir.
    if(CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        return()
    endif()

    foreach(_pair ${ARG_ASSETS})
        string(REPLACE ":" ";" _parts "${_pair}")
        list(GET _parts 0 _file)
        list(GET _parts 1 _symbol)
        set(_in "${CMAKE_CURRENT_LIST_DIR}/${_file}")
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_symbol}.c")

        execute_process(
            COMMAND python3 "${_script}" "${_in}" "${_out}" "${_symbol}"
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

# bb_embed_site(OUT_SRCS <var> TABLE <sym> DIST_DIR <dir> [URL_PREFIX <p>])
#
# Walks <dir> at CMake CONFIGURE time, calls gen_site.py to produce one gzipped
# blob .c per file plus a <sym>_table.c with the bb_http_asset_t[] table and
# lazy accessor.  Appends all generated .c paths to <OUT_SRCS> in caller scope.
#
# Reconfigures automatically when any file under DIST_DIR changes
# (CONFIGURE_DEPENDS via file(GLOB_RECURSE)).
#
# DIST_DIR resolves relative to CMAKE_CURRENT_LIST_DIR (caller's directory).
#
# Generated C API:
#   bb_http_asset_t <sym>[];                      // mutable table (len filled at runtime)
#   const bb_http_asset_t *<sym>_get(size_t *n);  // idempotent lazy accessor

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
    set(_script "${_bb_cmake_dir}/../scripts/gen_site.py")

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
        COMMAND python3 "${_script}"
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
        message(FATAL_ERROR "bb_embed_site: gen_site.py failed for ${ARG_TABLE}: ${_stderr}")
    endif()

    # Split stdout (one absolute .c path per line) into a CMake list
    string(REPLACE "\n" ";" _gen "${_stdout}")

    # Register dist dir contents as configure-time dependencies
    file(GLOB_RECURSE _site_inputs CONFIGURE_DEPENDS "${_dist_abs}/*")

    set(${ARG_OUT_SRCS} ${_gen} PARENT_SCOPE)
endfunction()
