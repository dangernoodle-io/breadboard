#!/usr/bin/env bash
# warn_scan.sh <host|firmware> -- produce the live -Wall build-artifact
# report that scripts/bbtool/fence/_gcc_warnings.py reads (B1-1420 PR2).
# `.github/workflows/build.yml` invokes this exact script in CI (the `test`
# job for `host`, the `smoke` job's `esp32` matrix leg for `firmware`) --
# this is not a separate local-only tool, it's the one and only report
# generator, used both there and on a dev machine.
#
# Neither platformio.ini's nor examples/smoke/CMakeLists.txt's committed
# -Werror is ever modified on disk: `host` builds against a temporary ini
# overlay (-c), and `firmware` patches examples/smoke/CMakeLists.txt only
# for the duration of the build, restoring the original via a trap before
# exiting (success or failure).
#
# Report contract (see _gcc_warnings.py's module docstring for the reader
# side): line 1 is always "# bb-warn-scan v1 target=<T> build_exit=<N>",
# written UNCONDITIONALLY -- including when the capture build itself fails
# -- so the fence's fail-closed check always has a real signal to read
# rather than a silently-missing/empty file.
#
# host needs a genuine-GNU gcc (scripts/coverage_toolchain.sh resolves one
# -- B1-642/B1-867); firmware needs the xtensa ESP-IDF toolchain (via
# PlatformIO). This script propagates the underlying build's real exit
# code -- a non-zero exit here means a genuine compile break, not "some
# warnings", since -Wno-error already converts warnings to non-fatal.
set -uo pipefail

TARGET="${1:-}"
if [ "$TARGET" != "host" ] && [ "$TARGET" != "firmware" ]; then
    echo "usage: $0 <host|firmware>" >&2
    exit 64
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPORT_DIR="$ROOT/.baseline/bbtool/fence/warning_fence_reports"
mkdir -p "$REPORT_DIR"
REPORT_FILE="$REPORT_DIR/$TARGET.log"
RAW_LOG="$(mktemp "${TMPDIR:-/tmp}/bb-warn-scan-$TARGET.XXXXXX.log")"

write_report() {
    local build_exit="$1"
    {
        echo "# bb-warn-scan v1 target=$TARGET build_exit=$build_exit"
        grep -E ': warning:|: In function |: At top level:' "$RAW_LOG" | sed "s#$ROOT/##g"
    } > "$REPORT_FILE"
}

if [ "$TARGET" = "host" ]; then
    TMP_INI="$(mktemp "${TMPDIR:-/tmp}/bb-warn-scan-host.XXXXXX.ini")"
    cleanup() { rm -f "$TMP_INI" "$RAW_LOG"; rm -rf "$ROOT/.pio/build/native"; }
    trap cleanup EXIT
    sed 's/^build_src_flags = -Werror$/build_src_flags = -Wall -Wno-error/' "$ROOT/platformio.ini" > "$TMP_INI"
    # Force a full recompile -- PlatformIO's incremental build otherwise
    # skips re-invoking gcc (and re-emitting diagnostics) for objects it
    # already built with equivalent flags in a prior run/worktree state.
    # This ALSO guarantees the report is never contaminated by a stale
    # prior build's flags, and the `cleanup` trap removes it again
    # afterward so the real `-Werror` + `--coverage` build that follows in
    # the same CI job starts genuinely fresh.
    rm -rf "$ROOT/.pio/build/native"
    "$ROOT/scripts/coverage_toolchain.sh" pio test -e native -c "$TMP_INI" --without-testing -d "$ROOT" \
        > "$RAW_LOG" 2>&1
    BUILD_EXIT=$?
else
    CMAKE_FILE="$ROOT/examples/smoke/CMakeLists.txt"
    cp "$CMAKE_FILE" "$CMAKE_FILE.warn-scan-orig"
    cleanup() {
        mv -f "$CMAKE_FILE.warn-scan-orig" "$CMAKE_FILE"
        rm -f "$RAW_LOG"
        rm -rf "$ROOT/examples/smoke/.pio/build/esp32"
    }
    trap cleanup EXIT
    sed -i.bak 's/target_compile_options("\${comp_lib}" PRIVATE -Werror)/target_compile_options("${comp_lib}" PRIVATE -Wall -Wno-error)/' "$CMAKE_FILE"
    rm -f "$CMAKE_FILE.bak"
    rm -rf "$ROOT/examples/smoke/.pio/build/esp32"
    ( cd "$ROOT" && make smoke-esp32 ) > "$RAW_LOG" 2>&1
    BUILD_EXIT=$?
fi

write_report "$BUILD_EXIT"
LINE_COUNT=$(wc -l < "$REPORT_FILE" | tr -d ' ')

if [ "$BUILD_EXIT" -ne 0 ]; then
    echo "warn_scan.sh: $TARGET -Wall capture build FAILED (exit=$BUILD_EXIT)" \
         "-- wrote $REPORT_FILE ($LINE_COUNT line(s), build_exit=$BUILD_EXIT" \
         "header -- the fence reads this as a hard fail in CI)" >&2
    tail -60 "$RAW_LOG" >&2
    exit "$BUILD_EXIT"
fi

echo "warn_scan.sh: wrote $REPORT_FILE ($LINE_COUNT line(s))"
