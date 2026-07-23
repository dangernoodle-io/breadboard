#!/usr/bin/env python3
"""Coverage measurement + gate (B1-642, B1-764, B1-867).

Runs gcovr over the host-test .gcda/.gcno data and gates on the committed
coverage baseline (scripts/coverage_baseline.py) -- Coveralls (the actual
merge gate) gates on LINES, but the local gate historically only checked
branches (`--txt-metric branch`), letting a line-only gap (PR #845: 100%
branches, 99.97% lines -- 3 uncovered lines) sail through locally and fail
only in CI; this gate checks both.

B1-764: FILTERS used to be a hand-maintained per-component allowlist that
only grew when someone happened to touch a component -- 90 of 111
host-compiled files were never admitted at all and silently reported as
100% (not "not measured", just absent from the report). FILTERS now admits
every `components/` and `platform/host/` file unconditionally, so a file
can never again land unmeasured by omission. The previously-unmeasured
files are, honestly measured, not fully covered -- see
scripts/coverage_baseline.py for how the gate handles that (shrink-only
per-file/per-LINE baseline instead of a backfill epic; branch coverage is
still measured and printed below but deliberately not per-marker
ratcheted -- gcov's branch-edge ids are not stable across gcc majors, see
scripts/coverage_baseline.py's module docstring).

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

import coverage_baseline

FILTERS = [
    "components/",
    "platform/host/",
    r"host_tools/bb_serialize_meta/bb_serialize_meta_validate\.c",
    r"host_tools/bb_serialize_meta/bb_serialize_meta_openapi\.c",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--coveralls-out", default="gcovr-coveralls.json")
    parser.add_argument("--summary-out", default="gcovr-summary.json")
    parser.add_argument("--detail-out", default="gcovr-detail.json",
                         help="per-line/per-branch gcovr `--json` report, consumed by the"
                              " coverage baseline ratchet (scripts/coverage_baseline.py)")
    baseline_group = parser.add_mutually_exclusive_group()
    baseline_group.add_argument(
        "--update-baseline", action="store_true",
        help="shrink-only: prune baseline entries no longer uncovered; never blesses"
             " a net-new gap (use --seed-baseline for the one-time initial baseline)")
    baseline_group.add_argument(
        "--seed-baseline", action="store_true",
        help="one-time: bless the current uncovered set wholesale as the starting"
             " baseline (errors if a baseline already exists)")
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
        "--json", args.detail_out,
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

    print(f"coverage_gate: line={line_percent:.2f}%, branch={branch_percent:.2f}% "
          "(aggregate, both measured/reported -- but only LINES are per-marker "
          "ratcheted by coverage_baseline; branch coverage has no stable "
          "per-marker identity across gcc majors, see scripts/coverage_baseline.py)")

    with open(args.detail_out) as fh:
        detail = json.load(fh)
    current = coverage_baseline.build_markers(detail)

    if args.seed_baseline:
        existing = coverage_baseline.baseline_path(args.root)
        if existing.is_file():
            print(f"coverage_gate: baseline already exists at {existing} -- use"
                  " --update-baseline to prune it, --seed-baseline is one-time only",
                  file=sys.stderr)
            return 1
        written = coverage_baseline.seed(args.root, current)
        print(f"coverage_gate: baseline seeded ({len(current)} entries) -> {written}")
        return 0

    if args.update_baseline:
        if not coverage_baseline.baseline_path(args.root).is_file():
            print("coverage_gate: no baseline yet -- use --seed-baseline first",
                  file=sys.stderr)
            return 1
        coverage_baseline.update_baseline(args.root, current)
        return 0

    return 0 if coverage_baseline.check(args.root, current) else 1


if __name__ == "__main__":
    sys.exit(main())
