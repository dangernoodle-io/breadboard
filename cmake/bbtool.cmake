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
#
# bb_lint() is invoked automatically below (ROOT=<breadboard_root>,
# PROFILE=library — identical to `make check`), so every build lints via the
# exact same rules as the CI/host gate. Set BB_LINT_ON_BUILD=OFF to opt out
# (deliberate override only).

cmake_minimum_required(VERSION 3.16)

option(BB_LINT_ON_BUILD "Run bbtool.py lint as part of every build; fails the build on any lint error. OFF disables (deliberate override)." ON)

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

# Assumes scripts/ ships alongside cmake/bbtool.cmake in every consumption
# path — true today (bb is consumed as a full checkout via
# EXTRA_COMPONENT_DIRS / brood's fetch_breadboard.py shallow-clone) but would
# break if bb were ever published as a trimmed registry package without dev
# tooling.
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
# Creates a custom target that runs breadboard's conventions linter via
# bbtool.py lint, AND (when BB_LINT_ON_BUILD is ON, the default) also runs
# the identical lint synchronously at CMake CONFIGURE time, failing configure
# (message(FATAL_ERROR)) on a non-zero lint exit.
#
# Both mechanisms exist because this repo is built two different ways:
#   - Plain CMake / idf.py: add_custom_target(... ALL ...) runs as part of
#     `cmake --build`, so the ALL wiring alone is sufficient there.
#   - PlatformIO's ESP-IDF integration: it runs `cmake -S/-B -G Ninja` to
#     configure and reads the CMake File API codemodel, but never invokes
#     `cmake --build`/ninja on the ALL target set — compilation happens via
#     PlatformIO's own SCons env. The ALL target is therefore inert under
#     `pio run`. The configure-time execute_process() below is what actually
#     enforces the gate in that flow.
# Together: the gate runs no matter which build driver invokes this file.
#
# On plain CMake / idf.py builds both mechanisms run (configure-time once,
# ALL-target once per build), which is by design, not an oversight: the
# lint itself is a cheap, fast static check, so paying for it twice is
# negligible next to the guarantee that no build driver can slip through
# unlinted.
#
# Set BB_LINT_ON_BUILD=OFF to disable both (deliberate override); the target
# still exists for manual invocation:
#   cmake --build <build_dir> --target <TARGET>
#
# Defaults: ROOT=${CMAKE_SOURCE_DIR}, PROFILE=consumer, TARGET=bb_lint.
# Guard: if the target already exists this call is a no-op, so multiple
# include()s or nested projects won't double-define the target or re-run
# the configure-time lint.
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

    # ESP-IDF's early-expansion pass (component_get_requirements.cmake) sets
    # CMAKE_BUILD_EARLY_EXPANSION and include()s every component CMakeLists.txt
    # solely to harvest REQUIRES/PRIV_REQUIRES, before the real build graph
    # exists. add_custom_target() is not valid in that pass ("not scriptable").
    # Skip; the real configure pass that follows defines the target normally.
    if(CMAKE_BUILD_EARLY_EXPANSION)
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

    set(_bb_lint_all "")
    if(BB_LINT_ON_BUILD)
        set(_bb_lint_all ALL)
    endif()

    add_custom_target(${ARG_TARGET} ${_bb_lint_all}
        COMMAND "${_py}" "${_script}" lint --root "${ARG_ROOT}" --profile "${ARG_PROFILE}"
        COMMENT "bb_lint: checking ${ARG_ROOT} (profile=${ARG_PROFILE})"
        USES_TERMINAL
    )

    if(BB_LINT_ON_BUILD)
        message(STATUS "bb_lint: running scripts/bbtool.py lint --root ${ARG_ROOT} --profile ${ARG_PROFILE} (configure-time gate)")
        execute_process(
            COMMAND "${_py}" "${_script}" lint --root "${ARG_ROOT}" --profile "${ARG_PROFILE}"
            RESULT_VARIABLE _bb_lint_rc
            OUTPUT_VARIABLE _bb_lint_out
            ERROR_VARIABLE _bb_lint_err
        )
        if(NOT _bb_lint_rc EQUAL 0)
            message(FATAL_ERROR
                "bb_lint: ${ARG_ROOT} (profile=${ARG_PROFILE}) failed:\n${_bb_lint_out}${_bb_lint_err}")
        endif()
        message(STATUS "bb_lint: OK (${ARG_ROOT}, profile=${ARG_PROFILE})")
    endif()
endfunction()

# Auto-invoke: only fires when the tree being configured is bb's OWN (smoke
# examples, host tests) — ROOT=<breadboard_root> PROFILE=library, the exact
# command `make check` runs, so there is zero rule divergence between the
# per-compile gate and the CI/host `make check` path.
#
# Downstream consumers (TM/brood/snugfeather, via bb_prov_default_form or any
# other component that include()s this file) must NOT get bb's whole-tree
# library-profile lint gated into their build — that's bb's own CI's job, not
# theirs. Scope with a path-boundary match: bb's own CMAKE_SOURCE_DIR (smoke
# examples, host tests) resolves under _BB_ROOT; a consumer's CMAKE_SOURCE_DIR
# (their app dir) does not.
#
# Two robustness fixes over a naive string(FIND ... EQUAL 0) prefix check:
#
#  1. Path-boundary safety: string(FIND ... EQUAL 0) is a character-prefix
#     match, not a path-component match, so a sibling directory that
#     string-extends bb's root name would false-positive — e.g.
#     _BB_ROOT=".../breadboard" vs. CMAKE_SOURCE_DIR=
#     ".../breadboard-wt-b1492-reclaim" (this org's worktree naming
#     convention makes that a real collision, not hypothetical). Require an
#     exact match OR that the character immediately following the _BB_ROOT
#     prefix is a path separator.
#
#  2. Symlink normalization: get_filename_component(... ABSOLUTE) does not
#     resolve symlinks. On macOS /tmp is a symlink to /private/tmp, and our
#     own worktrees live under /private/tmp/... — if one side of the
#     comparison resolves through the symlink and the other doesn't, the
#     literal strings diverge and this guard silently FALSE-NEGATIVES,
#     skipping both the ALL-target lint and the configure-time FATAL_ERROR
#     gate for bb's own tree with no warning. Normalize both sides to real
#     paths before comparing.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
    file(REAL_PATH "${_BB_ROOT}" _bb_root_real)
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _bb_source_dir_real)
else()
    get_filename_component(_bb_root_real "${_BB_ROOT}" REALPATH)
    get_filename_component(_bb_source_dir_real "${CMAKE_SOURCE_DIR}" REALPATH)
endif()

set(_bb_is_own_tree FALSE)
if(_bb_source_dir_real STREQUAL _bb_root_real)
    set(_bb_is_own_tree TRUE)
else()
    string(LENGTH "${_bb_root_real}" _bb_root_real_len)
    string(LENGTH "${_bb_source_dir_real}" _bb_source_dir_real_len)
    if(_bb_source_dir_real_len GREATER _bb_root_real_len)
        string(SUBSTRING "${_bb_source_dir_real}" 0 ${_bb_root_real_len} _bb_source_dir_prefix)
        string(SUBSTRING "${_bb_source_dir_real}" ${_bb_root_real_len} 1 _bb_next_char)
        if(_bb_source_dir_prefix STREQUAL _bb_root_real AND _bb_next_char STREQUAL "/")
            set(_bb_is_own_tree TRUE)
        endif()
    endif()
endif()

if(_bb_is_own_tree)
    message(STATUS "bbtool: auto-lint enabled for bb's own tree (CMAKE_SOURCE_DIR=${_bb_source_dir_real}, _BB_ROOT=${_bb_root_real})")
    bb_lint(ROOT "${_BB_ROOT}" PROFILE "library")
else()
    message(STATUS "bbtool: auto-lint skipped (consumer build) (CMAKE_SOURCE_DIR=${_bb_source_dir_real}, _BB_ROOT=${_bb_root_real})")
endif()
