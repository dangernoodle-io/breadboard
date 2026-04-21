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
