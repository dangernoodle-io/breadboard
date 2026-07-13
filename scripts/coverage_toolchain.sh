#!/usr/bin/env bash
# Resolve, verify, and PATH-shim a matched, genuine-GNU gcc/g++/gcov triple
# for host coverage (B1-642, B1-867), then run the given command under it.
#
# Why this exists (do not delete without reading):
#   - PlatformIO's native platform builder unconditionally deletes CC/CXX
#     from its SCons env and re-detects via `env.Tool("gcc")`/`env.Tool("g++")`
#     (~/.platformio/platforms/native/builder/main.py). Setting
#     `CC=gcc-16 make test` therefore does NOTHING -- PIO ignores it and
#     silently builds with whatever "gcc"/"g++" resolve to on PATH. The only
#     way to force a specific compiler is to make THAT be what "gcc"/"g++"
#     resolve to on PATH -- hence the shim directory below (B1-642).
#   - Apple's Xcode-bundled `gcov` (an LLVM tool that happens to be named
#     `gcov`) cannot read GNU gcc's .gcno/.gcda notes. It does not error --
#     gcovr just reports "All coverage data is filtered out" and/or a 0%
#     result. If Apple's gcov is ever resolved instead of a genuine GNU one,
#     every downstream number is meaningless (B1-867).
#
# The REAL invariants (there is no requirement that the major version be any
# particular number -- "16" was an artifact of one dev machine's Homebrew
# install, not a portability requirement):
#   1. The resolved gcov must be genuinely GNU, not Apple/clang/LLVM
#      masquerading under a gcc-compatible name (B1-867).
#   2. The compiler that produced the .gcno/.gcda notes and the gcov that
#      reads them must be the SAME MAJOR VERSION as each other -- a mismatch
#      yields "Version mismatch gcc/gcov" or a garbage/zeroed report.
# On Linux CI (ubuntu-latest installs no gcc at all -- the system default is
# whatever Ubuntu ships, e.g. gcc 13), unversioned gcc/gcov are a matched,
# genuine-GNU pair and satisfy both invariants perfectly. On macOS, the
# unversioned "gcc" IS clang, so a real GNU install (any major -- Homebrew's
# gcc-16 happens to be what's on this machine) is required; that's a LOCAL
# necessity, not a reproduction of CI's toolchain choice.
#
# Usage:
#   scripts/coverage_toolchain.sh <command> [args...]
#
# Env overrides (rarely needed):
#   COVERAGE_CC / COVERAGE_CXX / COVERAGE_GCOV   -- exact binary names/paths to require (skips auto-discovery for that tool)
#   COVERAGE_GCC_MAJOR                            -- OPT-IN: pin an exact expected GNU major version (unset by default -- any matched major is accepted)
#   COVERAGE_ALLOW_ANY_TOOLCHAIN=1                -- skip verification (loud warning, not silent)
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 64
fi

PIN_MAJOR="${COVERAGE_GCC_MAJOR:-}"
OVERRIDE_CC="${COVERAGE_CC:-}"
OVERRIDE_CXX="${COVERAGE_CXX:-}"
OVERRIDE_GCOV="${COVERAGE_GCOV:-}"

# tool_is_gnu <bin> -- true if <bin> --version does NOT report clang/LLVM/Apple.
tool_is_gnu() {
    local bin="$1" out
    out="$("$bin" --version 2>&1 | head -1)" || return 1
    ! printf '%s' "$out" | grep -qiE 'clang|llvm|apple'
}

# tool_major <bin> -- print <bin>'s major version (e.g. "16"), or fail.
tool_major() {
    local bin="$1" out ver
    out="$("$bin" --version 2>&1 | head -1)" || return 1
    ver="$(printf '%s' "$out" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | tail -1)"
    [ -n "$ver" ] || return 1
    printf '%s\n' "${ver%%.*}"
}

# gcc_candidate_names -- every "gcc-N" name found on PATH (highest N first),
# then the unversioned "gcc" name last. Not a hardcoded version list --
# discovered live from whatever is actually installed.
gcc_candidate_names() {
    { compgen -c 'gcc-' 2>/dev/null || true; } | grep -E '^gcc-[0-9]+$' | sort -t- -k2,2nr -u
    printf 'gcc\n'
}

RESOLVED_CC="" RESOLVED_CXX="" RESOLVED_GCOV="" RESOLVED_MAJOR=""

resolve_toolchain() {
    local cc_candidates
    if [ -n "$OVERRIDE_CC" ]; then
        cc_candidates="$OVERRIDE_CC"
    elif [ -n "$PIN_MAJOR" ]; then
        cc_candidates="gcc-${PIN_MAJOR}"
    else
        cc_candidates="$(gcc_candidate_names)"
    fi

    local cc_name cc_path cc_major cxx_name cxx_path cxx_major gcov_name gcov_path gcov_major
    while IFS= read -r cc_name; do
        [ -n "$cc_name" ] || continue
        cc_path="$(command -v "$cc_name" 2>/dev/null)" || continue
        tool_is_gnu "$cc_path" || continue
        cc_major="$(tool_major "$cc_path")" || continue
        if [ -n "$PIN_MAJOR" ] && [ "$cc_major" != "$PIN_MAJOR" ]; then
            continue
        fi

        if [ -n "$OVERRIDE_CXX" ]; then
            cxx_name="$OVERRIDE_CXX"
        elif [ "$cc_name" = "${cc_name#gcc-}" ]; then
            cxx_name="g++"
        else
            cxx_name="g++-${cc_name#gcc-}"
        fi
        cxx_path="$(command -v "$cxx_name" 2>/dev/null)" || continue
        tool_is_gnu "$cxx_path" || continue
        cxx_major="$(tool_major "$cxx_path")" || continue
        [ "$cxx_major" = "$cc_major" ] || continue

        if [ -n "$OVERRIDE_GCOV" ]; then
            gcov_path="$(command -v "$OVERRIDE_GCOV" 2>/dev/null)" || continue
        else
            if [ "$cc_name" = "${cc_name#gcc-}" ]; then
                gcov_name="gcov"
            else
                gcov_name="gcov-${cc_name#gcc-}"
            fi
            gcov_path="$(command -v "$gcov_name" 2>/dev/null)" || true
            if [ -z "$gcov_path" ]; then
                gcov_path="$(command -v gcov 2>/dev/null)" || continue
            fi
        fi
        tool_is_gnu "$gcov_path" || continue
        gcov_major="$(tool_major "$gcov_path")" || continue
        [ "$gcov_major" = "$cc_major" ] || continue

        RESOLVED_CC="$cc_path"
        RESOLVED_CXX="$cxx_path"
        RESOLVED_GCOV="$gcov_path"
        RESOLVED_MAJOR="$cc_major"
        return 0
    done <<EOF
$cc_candidates
EOF
    return 1
}

fail_unresolved() {
    cat >&2 <<EOF
coverage_toolchain: could not resolve a matched, genuine-GNU gcc/g++/gcov toolchain triple.

Two real invariants are required -- there is no requirement on the specific
major version number:
  1. gcov must be genuinely GNU, not Apple's clang/LLVM masquerading under a
     gcc-compatible name (B1-867).
  2. The compiler and the gcov must share the SAME MAJOR VERSION as each
     other (a mismatch produces "Version mismatch gcc/gcov" or a garbage/
     zeroed report) (B1-642).

On macOS, Apple's default cc/gcov are clang and fail invariant 1 -- install a
real GNU toolchain, e.g.: brew install gcc

To pin an exact major version (rarely needed -- e.g. multiple GNU majors
installed and you want a specific one), set COVERAGE_GCC_MAJOR=<N>. This is
opt-in; no major is required by default.

To bypass this check entirely (NOT recommended -- produces unverified numbers):
  COVERAGE_ALLOW_ANY_TOOLCHAIN=1
EOF
    exit 1
}

if [ "${COVERAGE_ALLOW_ANY_TOOLCHAIN:-0}" != "1" ]; then
    resolve_toolchain || fail_unresolved
else
    echo "==========================================================================" >&2
    echo "coverage_toolchain: COVERAGE_ALLOW_ANY_TOOLCHAIN=1 -- SKIPPING GNU-toolchain verification." >&2
    echo "Results below are UNVERIFIED and may be silently wrong (B1-642, B1-867)." >&2
    echo "==========================================================================" >&2
    RESOLVED_CC="${OVERRIDE_CC:-$(command -v gcc)}"
    RESOLVED_CXX="${OVERRIDE_CXX:-$(command -v g++)}"
    RESOLVED_GCOV="${OVERRIDE_GCOV:-$(command -v gcov)}"
fi

# PlatformIO's native builder deletes CC/CXX from its SCons env and re-detects
# via `env.Tool("gcc")`/`env.Tool("g++")`, which searches PATH for "gcc"/"cc"
# and "g++"/"c++" respectively -- so the only way to steer it is to shadow
# those exact names on PATH ahead of the real ones.
SHIM_DIR="$(mktemp -d "${TMPDIR:-/tmp}/bb-coverage-toolchain.XXXXXX")"
trap 'rm -rf "$SHIM_DIR"' EXIT

ln -s "$RESOLVED_CC"   "$SHIM_DIR/gcc"
ln -s "$RESOLVED_CC"   "$SHIM_DIR/cc"
ln -s "$RESOLVED_CXX"  "$SHIM_DIR/g++"
ln -s "$RESOLVED_CXX"  "$SHIM_DIR/c++"
ln -s "$RESOLVED_GCOV" "$SHIM_DIR/gcov"

export PATH="$SHIM_DIR:$PATH"
export CC="$RESOLVED_CC"
export CXX="$RESOLVED_CXX"
export COVERAGE_RESOLVED_GCOV="$RESOLVED_GCOV"

# Deliberately NOT `exec "$@"`: a successful exec replaces this process image,
# so the EXIT trap above never runs and SHIM_DIR leaks on every normal
# invocation (bash does not run EXIT traps through a successful exec). Run as
# a child instead so the trap fires -- on both success and failure -- and the
# tempdir is always cleaned up.
set +e
"$@"
status=$?
set -e
exit "$status"
