# bb_embed_assets(OUT_SRCS <var> ASSETS <file>:<symbol> [<file>:<symbol> ...])
#
# For each "<file>:<symbol>" pair, adds an add_custom_command that invokes
# breadboard/scripts/embed_html.py to generate <CMAKE_CURRENT_BINARY_DIR>/<symbol>.c
# at build time. The generated .c paths are appended to <OUT_SRCS> in the caller's
# scope; the caller must list ${OUT_SRCS} in SRCS of idf_component_register.
#
# MUST be called BEFORE idf_component_register so OUT_SRCS is populated in time.
# The add_custom_command lines themselves are emitted by this function, so the
# ESP-IDF script-mode requirements pass does not see them at the component
# CMakeLists top level — moving the "not scriptable" risk into the function
# body (still called at top level). If script-mode rejects function bodies too,
# guard the add_custom_command calls with `if(NOT CMAKE_SCRIPT_MODE_FILE)`.
#
# Input file paths are resolved relative to CMAKE_CURRENT_LIST_DIR of the caller.

function(bb_embed_assets)
    cmake_parse_arguments(ARG "" "OUT_SRCS" "ASSETS" ${ARGN})

    # Locate the script relative to this file's directory.
    if(NOT DEFINED BB_EMBED_SCRIPT)
        # Use the directory of this CMake file (bb_embed.cmake), not the caller.
        get_filename_component(_bb_cmake_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
        set(BB_EMBED_SCRIPT "${_bb_cmake_dir}/../scripts/embed_html.py" CACHE FILEPATH "breadboard embed_html.py script")
    endif()

    set(_gen)

    # Parse each "file:symbol" pair and emit add_custom_command.
    foreach(_asset ${ARG_ASSETS})
        string(FIND "${_asset}" ":" _colon_pos)
        if(_colon_pos EQUAL -1)
            message(FATAL_ERROR "bb_embed_assets: asset '${_asset}' must have format 'file:symbol'")
        endif()

        string(SUBSTRING "${_asset}" 0 ${_colon_pos} _file)
        math(EXPR _symbol_start "${_colon_pos} + 1")
        string(SUBSTRING "${_asset}" ${_symbol_start} -1 _symbol)

        # Resolve input path relative to the caller's CMakeLists.txt directory.
        set(_input_path "${CMAKE_CURRENT_LIST_DIR}/${_file}")
        set(_output_path "${CMAKE_CURRENT_BINARY_DIR}/${_symbol}.c")

        # Add the custom command (only during normal cmake, not script-mode passes).
        if(NOT CMAKE_SCRIPT_MODE_FILE)
            add_custom_command(
                OUTPUT "${_output_path}"
                COMMAND python3 "${BB_EMBED_SCRIPT}" "${_input_path}" "${_output_path}" "${_symbol}"
                DEPENDS "${_input_path}" "${BB_EMBED_SCRIPT}"
                VERBATIM
            )
        endif()

        # Append to the output list.
        list(APPEND _gen "${_output_path}")
    endforeach()

    # Set the output variable in the caller's scope.
    set(${ARG_OUT_SRCS} ${_gen} PARENT_SCOPE)
endfunction()
