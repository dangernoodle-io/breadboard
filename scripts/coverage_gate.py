#!/usr/bin/env python3
"""Coverage measurement + gate (B1-642, B1-867).

Runs gcovr over the host-test .gcda/.gcno data and gates on BOTH line and
branch coverage -- Coveralls (the actual merge gate) gates on LINES, but the
local gate historically only checked branches (`--txt-metric branch`),
letting a line-only gap (PR #845: 100% branches, 99.97% lines -- 3 uncovered
lines) sail through locally and fail only in CI.

This script also treats an exactly-zero (or "no relevant lines found")
result as a TOOLING FAILURE, not a real coverage number -- Apple's `gcov`
masquerading under a GNU-toolchain name silently emits a zeroed/bogus
report instead of erroring (B1-867); a genuine 0% across an entire repo with
thousands of passing tests is never a real result.

Must be run under scripts/coverage_toolchain.sh so PATH/CC/CXX/
COVERAGE_RESOLVED_GCOV are already a verified GNU toolchain -- this script
does not itself verify the toolchain.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys

FILTERS = [
    "components/",
    r"platform/espidf/bb_cache/",
    r"platform/host/bb_cache/",
    r"platform/espidf/bb_cache_reactive/",
    r"platform/host/bb_cache_reactive/",
    r"platform/espidf/bb_cache_serialize/",
    r"platform/host/bb_cache_serialize/",
    r"platform/host/bb_sink_display/",
    r"platform/host/bb_cache_routes/",
    r"platform/host/bb_mdns_cache/",
    r"platform/host/bb_str/",
    r"platform/host/bb_scalar/",
    r"platform/host/bb_num/",
    r"platform/host/bb_fmt/",
    r"platform/host/bb_core/bb_clock\.c",
    r"platform/host/bb_core/bb_lock\.c",
    r"platform/host/bb_core/bb_lock_cond\.c",
    r"platform/host/bb_core/bb_lock_impl\.h",
    r"platform/host/bb_meminfo/",
    r"platform/host/bb_mem_arena/",
    r"platform/host/bb_bqueue/",
    r"test/test_host/bb_serialize_meta_validate\.c",
    r"test/test_host/bb_serialize_meta_openapi\.c",
]

# DECISION: this repo's real merge gate is Coveralls, which gates on
# NON-REGRESSION against the base branch, not a literal 100% -- but this
# local gate deliberately checks a stricter, absolute 100%/100% floor
# instead of trying to reproduce a non-regression diff locally (no stored
# baseline to diff against here). This is a house rule, chosen ON PURPOSE,
# not an accidental stand-in for Coveralls: the repo is at 100%/100% today,
# and a hard floor is simpler to reason about locally and catches a
# regression before it ever reaches CI. If base coverage is ever legitimately
# below 100% (an approved exception, an intentionally-excluded component),
# override via COVERAGE_MIN_LINE/COVERAGE_MIN_BRANCH for a deliberate,
# reviewed exception -- never silently, and never by lowering these
# defaults without updating this comment.
DEFAULT_MIN_LINE = 100.0
DEFAULT_MIN_BRANCH = 100.0


def _env_float(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError:
        print(f"coverage_gate: invalid {name}={raw!r}, expected a number", file=sys.stderr)
        sys.exit(2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--coveralls-out", default="gcovr-coveralls.json")
    parser.add_argument("--summary-out", default="gcovr-summary.json")
    args = parser.parse_args()

    gcov_executable = os.environ.get("COVERAGE_RESOLVED_GCOV")

    cmd = ["gcovr", "--root", args.root]
    for f in FILTERS:
        cmd += ["--filter", f]
    cmd += [
        "--exclude-throw-branches",
        "--exclude-unreachable-branches",
        "--exclude-directories", r"\.claude",
        "--merge-mode-functions=merge-use-line-max",
        "--print-summary",
        "--coveralls", args.coveralls_out,
        "--json-summary", args.summary_out,
        "--txt",
    ]
    if gcov_executable:
        cmd += ["--gcov-executable", gcov_executable]
    elif not shutil.which("gcov"):
        print("coverage_gate: no gcov on PATH and COVERAGE_RESOLVED_GCOV unset -- "
              "run via scripts/coverage_toolchain.sh", file=sys.stderr)
        return 1

    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        print(f"coverage_gate: gcovr exited {proc.returncode}", file=sys.stderr)
        return proc.returncode

    with open(args.summary_out) as fh:
        summary = json.load(fh)

    line_total = summary.get("line_total", 0)
    branch_total = summary.get("branch_total", 0)
    line_percent = summary.get("line_percent", 0.0) or 0.0
    branch_percent = summary.get("branch_percent", 0.0) or 0.0

    # B1-867 guard: a real measurement over this repo's ~7000 covered lines
    # is never literally zero, and zero relevant lines means the filters/
    # instrumentation found nothing at all. Either is a tooling failure, not
    # a coverage result -- fail loudly and distinctly from a real regression.
    if line_total == 0 or branch_total == 0:
        print(
            "coverage_gate: ABORT -- gcovr found ZERO relevant lines/branches "
            f"(line_total={line_total}, branch_total={branch_total}).\n"
            "This is a tooling failure (wrong gcov, filters matched nothing, "
            "or .gcda/.gcno missing), not a real coverage result -- see B1-867. "
            "Re-run via scripts/coverage_toolchain.sh and confirm `make test` "
            "actually built (fresh `pio run -t clean` if stale).",
            file=sys.stderr,
        )
        return 1
    if line_percent == 0.0 or branch_percent == 0.0:
        print(
            "coverage_gate: ABORT -- gcovr reports exactly 0% "
            f"(line={line_percent}%, branch={branch_percent}%) over "
            f"{line_total} lines / {branch_total} branches. A genuine 0% "
            "over a repo with thousands of passing tests is not a real "
            "result -- this is the Apple-gcov masquerade failure mode "
            "(B1-867). Re-run via scripts/coverage_toolchain.sh.",
            file=sys.stderr,
        )
        return 1

    min_line = _env_float("COVERAGE_MIN_LINE", DEFAULT_MIN_LINE)
    min_branch = _env_float("COVERAGE_MIN_BRANCH", DEFAULT_MIN_BRANCH)

    print(f"coverage_gate: line={line_percent:.2f}% (min {min_line}%), "
          f"branch={branch_percent:.2f}% (min {min_branch}%)")

    ok = True
    if line_percent < min_line:
        print(f"coverage_gate: FAIL -- line coverage {line_percent:.2f}% "
              f"< required {min_line}% (Coveralls gates on lines)", file=sys.stderr)
        ok = False
    if branch_percent < min_branch:
        print(f"coverage_gate: FAIL -- branch coverage {branch_percent:.2f}% "
              f"< required {min_branch}%", file=sys.stderr)
        ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
