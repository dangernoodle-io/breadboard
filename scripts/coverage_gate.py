#!/usr/bin/env python3
"""Coverage measurement + gate (B1-642, B1-764, B1-867, B1-1258).

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
scripts/coverage_baseline.py for how the gate handles that: a shrink-only
per-file baseline instead of a backfill epic, now covering BOTH per-line
AND per-line-branch-existence markers (B1-1258 -- see
scripts/coverage_baseline.py's module docstring for why a branch marker is
keyed by line number only, never a branch-edge id, which is what makes it
safe to baseline despite gcov's edge ids not being stable across gcc
majors).

This script also treats an exactly-zero (or "no relevant lines found")
result as a TOOLING FAILURE, not a real coverage number -- Apple's `gcov`
masquerading under a GNU-toolchain name silently emits a zeroed/bogus
report instead of erroring (B1-867); a genuine 0% across an entire repo with
thousands of passing tests is never a real result.

Must be run under scripts/coverage_toolchain.sh so PATH/CC/CXX/
COVERAGE_RESOLVED_GCOV are already a verified GNU toolchain -- this script
does not itself verify the toolchain.

B1-1258 CI/local split: uncovered_branch is a LOCAL PRE-PUSH AID ONLY, not a
CI gate. CI found two branch markers (platform/host/bb_system/bb_system_host.c:94,
platform/host/bb_tls/bb_tls.c:21) that don't reproduce on a dev machine --
the 501-marker branch baseline was seeded under a local gcc, but the gate
also runs under CI's (different) gcc, and branch attribution (how many CFG
edges a compound conditional splits into, and which of them count as
"taken") is gcc-major-dependent -- this is exactly the instability PR #850
documented, and keying uncovered_branch by line number (see
scripts/coverage_baseline.py) narrows it but cannot eliminate it: two
different gcc majors can each split the SAME line into a different branch
COUNT, so "does this line have >=1 uncovered branch" can itself legitimately
differ. Line attribution has no such problem (a source line is a source
line regardless of gcc major), so uncovered_line keeps gating CI unchanged.
Coveralls (running the actual PR's own CI toolchain) remains the sole CI
authority for branch coverage. See COVERAGE_GATE_LINES_ONLY below.
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
]

# B1-1258: explicit CI opt-out for the uncovered_branch ratchet -- set by
# .github/workflows/build.yml's pre-build-script (never auto-detected from a
# generic $CI-style var; auto-detection would silently and non-greppably
# change behavior for anyone running `make coverage` inside a container).
# When set (to any value other than "", "0", or "false"), uncovered_branch
# markers are dropped from `current` before seed/update/check ever see them
# -- uncovered_line ratcheting is completely unaffected. Local runs (this
# env var unset) keep ratcheting both, which is the entire point of B1-1258:
# a genuine local pre-push regression check that CI cannot yet reliably
# reproduce across gcc majors.
LINES_ONLY_ENV = "COVERAGE_GATE_LINES_ONLY"


def lines_only_enabled() -> bool:
    return os.environ.get(LINES_ONLY_ENV, "") not in ("", "0", "false", "False")


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

    lines_only = lines_only_enabled()
    if lines_only:
        ratchet_note = (f"LINES ONLY are per-marker ratcheted by coverage_baseline "
                         f"({LINES_ONLY_ENV} is set -- see below)")
    else:
        ratchet_note = ("both LINE and per-line-BRANCH-existence markers are "
                         "per-marker ratcheted by coverage_baseline")
    print(f"coverage_gate: line={line_percent:.2f}%, branch={branch_percent:.2f}% "
          f"(aggregate -- {ratchet_note}, see scripts/coverage_baseline.py)")

    with open(args.detail_out) as fh:
        detail = json.load(fh)
    current = coverage_baseline.build_markers(detail)

    if lines_only:
        current = {m for m in current if m.type != "uncovered_branch"}
        print(
            f"coverage_gate: {LINES_ONLY_ENV} set -- uncovered_branch markers "
            "excluded from the baseline ratchet for this run (branch attribution "
            "is gcc-major-dependent, see module docstring; Coveralls is the CI "
            "authority for branch coverage; uncovered_line ratcheting is "
            "unaffected)"
        )

    if args.seed_baseline:
        try:
            written = coverage_baseline.seed(args.root, current)
        except coverage_baseline.SeedError as exc:
            print(f"coverage_gate: {exc}", file=sys.stderr)
            return 1
        total = len(coverage_baseline.load_baseline(args.root))
        print(f"coverage_gate: baseline seeded ({total} total entries) -> {written}")
        return 0

    if args.update_baseline:
        if not coverage_baseline.baseline_path(args.root).is_file():
            print("coverage_gate: no baseline yet -- use --seed-baseline first",
                  file=sys.stderr)
            return 1
        coverage_baseline.update_baseline(args.root, current)
        return 0

    ignore_types = frozenset({"uncovered_branch"}) if lines_only else frozenset()
    return 0 if coverage_baseline.check(args.root, current, ignore_types) else 1


if __name__ == "__main__":
    sys.exit(main())
