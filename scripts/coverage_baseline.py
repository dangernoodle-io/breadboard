#!/usr/bin/env python3
"""Coverage ratchet-baseline engine (B1-764).

B1-764 found the coverage gate had never measured 90 of 111 host-compiled
files (a per-component `--filter` allowlist that only ever grew when
someone happened to touch a component -- the exact allowlist-drift bug
class KB 1407 warns about). The fix admits every `components/` and
`platform/host/` file unconditionally (see FILTERS in coverage_gate.py) --
but the previously-unmeasured files are, honestly measured, not fully
covered. There is no backfill epic: tests written against a shape that is
about to be refactored are throwaway and actively obstruct the refactor
(several of these components were mid-retirement at the time -- e.g. the
bb_pub/bb_sink_* cluster, fully removed by B1-905). Instead:

  1. every file's CURRENT line gap is baselined (recorded as a floor), so
     the gate never again silently reports 100% over unmeasured code;
  2. the baseline is SHRINK-ONLY -- a gap may close, never reopen;
  3. a file with NO baseline entry (new file, or an existing file whose
     line numbers all shifted because it was edited) is held to 100%
     lines, because "no baseline entry" and "0% covered" look identical
     to the ratchet -- see the module docstring note on this below.

Originally LINES ONLY -- see the B1-1258 update below for why per-line
BRANCH markers were added back. The original line-only choice was also an
alignment win: Coveralls (the real merge gate) gates on lines, but the
local gate used to ratchet branches, which let a PR be 100% branches
locally yet still fail Coveralls's line gate (PR #845). A line-based local
baseline matches what actually blocks the merge.

This reuses scripts/bbtool/fence/_base.py's generic Marker/diff/baseline-
file engine -- the SAME shrink-only-baseline idiom already used by the
di_legacy/new_component fences (Marker(type, path, id); diff() computes
new-vs-removed by an identity key; a baseline JSON file is the committed
floor). It is deliberately NOT registered as an auto-discovered bbtool
fence family (dropping a module into scripts/bbtool/fence/ auto-enrolls it
in `bbtool fence`'s default "run every family" pass, which is `make
fence`, which is part of the fast, no-build `make check`). This family's
scanner requires a full instrumented `pio test` build under the coverage
toolchain PATH-shim -- a wildly different cost profile than every other
family's static regex scan over source text. Folding it into `make check`
would make a lint-speed target require a full coverage build every time.
So: reuse the engine, but keep this baseline out of
`.baseline/bbtool/fence/` (out of that auto-discovery's reach) at
`.baseline/coverage/coverage.json`, and gate it only from `make coverage`,
which already pays for the instrumented build.

Marker shape:
  - type="uncovered_line", path=<file>, id="<line_number>"
  - type="uncovered_branch", path=<file>, id="<line_number>"

B1-1258 -- uncovered_branch markers ARE baselined (this reverses the
"LINES ONLY" choice above, but NOT the reasoning that motivated it -- see
below for why this is compatible). PR #1120 found the previous line-only
gate false-passing a branch-only regression: 100% new-code LINE coverage,
but 4 new uncovered BRANCHES in a brand-new file
(components/bb_wifi_prov/src/bb_wifi_prov_page.c) that only Coveralls (the
real merge gate, which DOES gate on a decrease in aggregate branch %)
caught -- `make coverage` reported "0 new uncovered markers" and PASSED.
Diagnosing that required manually reading gcovr-detail.json; it should
instead have failed locally like any other regression.

CI/local split (still B1-1258, added one PR later): the residual risk noted
below DID materialize -- CI failed with two uncovered_branch markers
(bb_system_host.c:94, bb_tls.c:21) that never reproduced on the dev machine
that seeded the 501-entry branch baseline, because CI runs a different gcc
major and, per the residual-risk note below, "does this line have >=1
uncovered branch" can itself differ across majors even at the SAME line
number, not just the edge count. The user's call: uncovered_branch stays a
LOCAL PRE-PUSH AID ONLY, never a CI gate -- Coveralls (which runs under the
PR's own CI toolchain) is the sole CI authority for branch coverage.
uncovered_line ratcheting is untouched and still gates CI exactly as
before. The CI opt-out is `coverage_gate.py`'s `COVERAGE_GATE_LINES_ONLY`
env var, set explicitly by `.github/workflows/build.yml`'s
`pre-build-script` -- never auto-detected -- so branch ratcheting still
fully gates every local `make coverage` run, which is the entire point of
this ticket.

PR #850's reversal (see git history) is still correct that a *branch-edge*
identity (gcov's source_block_id/destination_block_id pair) is NOT stable
across gcc majors -- different majors split a compound conditional into a
different number of CFG edges for the identical source line, so a
baseline keyed on edge ids produces spurious "new" markers the moment
CI's gcc major differs from whichever seeded the baseline, for code
nobody touched. B1-1258's fix does not resurrect edge-id identity: an
uncovered_branch marker is keyed the SAME way as uncovered_line -- by
(type, path, source line number) only, with no reference to
source_block_id/destination_block_id/branch count at all. A line
contributes at most ONE uncovered_branch marker regardless of how many of
its branch edges gcov reports, or how gcc splits them -- the marker means
"this line has at least one not-fully-exercised branch", not "branch N of
this line is uncovered". Source line numbers ARE stable across gcc majors
(a source line is a source line regardless of compiler version), same as
uncovered_line's existing identity, so this sidesteps PR #850's actual
failure mode rather than re-triggering it. (The only residual risk --
that gcc's split changes which decision-outcomes IT reports as covered
for a genuinely-identical source line and test run -- is the same risk
already accepted for the aggregate branch percentage gcovr reports today;
this does not add a new one.)

Identity is (type, path, id) -- NOT di_legacy's default (type, id). A bare
line number collides across every file that happens to have an uncovered
line 5, so path must be part of the identity here (di_legacy's ids are
already-unique macro/component names, so it can afford the more
rename-tolerant default).

Practical effect ("touch it, bring it to 100", by construction, not by
policy): editing a function shifts every following line number. Nearly any
edit to a file with baseline gaps turns ALL of that file's still-open gaps
into "new" (line-number-shifted) markers -- which must be closed (or the
file deliberately re-seeded) before that PR's gate passes. A brand new file
has zero baseline entries, so any uncovered line in it is unconditionally
new. Neither is a special case in the code below -- both fall out of plain
set-diff-by-identity.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Set

_BBTOOL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bbtool")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

from fence import _base  # noqa: E402
from fence._base import Marker  # noqa: E402

BASELINE_REL_PATH = Path(".baseline") / "coverage" / "coverage.json"


def identity(m: Marker) -> tuple:
    """Path-sensitive identity -- see module docstring for why this
    overrides di_legacy's default (type, id)."""
    return (m.type, m.path, m.id)


def build_markers(detail: dict) -> Set[Marker]:
    """Turn a gcovr `--json` detail report into the current uncovered-line
    AND uncovered-branch marker sets (see module docstring, B1-1258, for why
    uncovered_branch is keyed by line number only -- never a branch-edge id).

    A line gcovr itself marks `gcovr/excluded` (LCOV_EXCL_LINE) is skipped
    entirely (both for its own uncovered_line marker AND for any branches on
    it) -- it already does not count toward gcovr's own line_percent, so
    counting it here too would baseline a false gap gcovr never actually
    reports as uncovered. A branch gcovr marks `gcovr/excluded` individually
    (e.g. LCOV_EXCL_BR_LINE) is skipped the same way, independent of its
    line's own exclusion/coverage state."""
    markers: Set[Marker] = set()
    for f in detail.get("files", []):
        path = f["file"]
        for line in f.get("lines", []):
            if line.get("gcovr/excluded"):
                continue
            line_number = str(line["line_number"])
            if line.get("count", 0) == 0:
                markers.add(Marker("uncovered_line", path, line_number))
            for branch in line.get("branches", []):
                if branch.get("gcovr/excluded"):
                    continue
                if branch.get("count", 0) == 0:
                    markers.add(Marker("uncovered_branch", path, line_number))
                    break  # one marker per line regardless of branch count
    return markers


def baseline_path(root: str) -> Path:
    return Path(root) / BASELINE_REL_PATH


def load_baseline(root: str) -> Set[Marker]:
    path = baseline_path(root)
    if not path.is_file():
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    return {Marker(e["type"], e["path"], e["id"]) for e in data.get("entries", [])}


def write_baseline(root: str, markers: Set[Marker]) -> Path:
    path = baseline_path(root)
    entries = [
        {"type": m.type, "path": m.path, "id": m.id}
        for m in sorted(markers, key=lambda m: (m.path, m.type, m.id))
    ]
    payload = {"entries": entries}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def diff(current: Set[Marker], baseline: Set[Marker]):
    """Returns (new_markers, removed_markers) -- see fence._base.diff."""
    return _base.diff(current, baseline, identity_fn=identity)


def check(root: str, current: Set[Marker], ignore_types: frozenset = frozenset()) -> bool:
    """PASS/FAIL check for `make coverage`: FAIL iff there is any marker in
    `current` not already in the committed baseline.

    `ignore_types` (B1-1258): baseline entries of these types are dropped
    from the comparison entirely -- neither reported as new (a regression)
    nor as a prune candidate (implying they got fixed this run). Used by the
    CI COVERAGE_GATE_LINES_ONLY opt-out: when `current` was pre-filtered to
    exclude uncovered_branch (coverage_gate.py), the baseline's 500+
    uncovered_branch entries would otherwise ALL show up as spurious "now
    covered, candidate to prune" INFO noise on every single CI run (they
    were simply never measured this run, not verified fixed) -- pass
    `ignore_types={"uncovered_branch"}` to suppress that instead of silently
    tolerating it."""
    baseline = load_baseline(root)
    if ignore_types:
        baseline = {m for m in baseline if m.type not in ignore_types}
    new, removed = diff(current, baseline)

    for m in removed:
        print(f"INFO [coverage-baseline]: candidate to prune (now covered): "
              f"{m.path}:{m.id} ({m.type})")

    if new:
        by_file: dict = {}
        for m in new:
            by_file.setdefault(m.path, []).append(m)
        for path, ms in sorted(by_file.items()):
            ids = ", ".join(f"{m.id} ({m.type})" for m in
                             sorted(ms, key=lambda m: (m.id, m.type)))
            print(f"ERROR [coverage-baseline]: {path}: {len(ms)} new uncovered "
                  f"line/branch marker(s) not in baseline: {ids}", file=sys.stderr)
        print(
            f"coverage_baseline: {len(new)} new uncovered marker(s) -- FAIL "
            "(new/changed code must reach 100% LINE and BRANCH coverage; a "
            "file with pre-existing baseline gaps must have ALL of them "
            "closed once you touch it -- see scripts/coverage_baseline.py)",
            file=sys.stderr,
        )
        return False

    print(f"coverage_baseline: {len(current)} known-uncovered line/branch "
          "marker(s) (baselined), 0 new -- PASS "
          "(see scripts/coverage_baseline.py)")
    return True


def update_baseline(root: str, current: Set[Marker]) -> Path:
    """Shrink-only: prune baseline entries that are no longer uncovered.
    Never adds a net-new marker -- a genuinely new gap always stays a
    failure until it is fixed or the file is deliberately re-seeded via
    seed()."""
    baseline = load_baseline(root)
    new, removed = diff(current, baseline)
    removed_ids = {identity(m) for m in removed}
    surviving = {m for m in baseline if identity(m) not in removed_ids}
    written = write_baseline(root, surviving)
    print(f"coverage_baseline: baseline pruned ({len(removed)} removed, "
          f"{len(surviving)} kept) -> {written}")
    if new:
        print(f"INFO [coverage-baseline]: {len(new)} new marker(s) NOT added "
              "(--update-baseline is shrink-only) -- still failing on a normal run")
    return written


class SeedError(RuntimeError):
    """Raised by seed() when there is nothing new to seed -- every marker
    TYPE present in `current` already has baseline entries of that type."""


def seed(root: str, current: Set[Marker]) -> Path:
    """Per-TYPE-additive one-time seed: bless the current uncovered set of
    any marker TYPE not yet present in the committed baseline (e.g. the
    B1-1258 one-time addition of uncovered_branch alongside an
    already-seeded uncovered_line baseline). Never touches a type that
    already has baseline entries -- that type's gaps are re-baselined only
    via the shrink-only --update-baseline, never re-seeded wholesale (which
    would silently bless any net-new gap of that type). Raises SeedError
    if `current` contains no marker of a type that isn't already baselined
    (covers both "no baseline yet, nothing uncovered" and "baseline fully
    up to date for every type already present" -- mirrors bbtool fence's
    --seed semantics of refusing a redundant reseed)."""
    existing = load_baseline(root)
    existing_types = {m.type for m in existing}
    new_type_markers = {m for m in current if m.type not in existing_types}
    if not new_type_markers:
        raise SeedError(
            "nothing to seed -- baseline already exists for every marker "
            "type in the current scan (use --update-baseline to prune, "
            "seed is per-type one-time only)"
        )
    return write_baseline(root, existing | new_type_markers)
