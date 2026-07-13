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
(several of these components, e.g. bb_pub/bb_sink_*, are mid-retirement --
B1-836). Instead:

  1. every file's CURRENT line gap is baselined (recorded as a floor), so
     the gate never again silently reports 100% over unmeasured code;
  2. the baseline is SHRINK-ONLY -- a gap may close, never reopen;
  3. a file with NO baseline entry (new file, or an existing file whose
     line numbers all shifted because it was edited) is held to 100%
     lines, because "no baseline entry" and "0% covered" look identical
     to the ratchet -- see the module docstring note on this below.

LINES ONLY, not branches -- this is also an alignment win: Coveralls (the
real merge gate) gates on lines, but the local gate used to ratchet
branches, which let a PR be 100% branches locally yet still fail
Coveralls's line gate (PR #845). A line-based local baseline now matches
what actually blocks the merge.

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

LINES ONLY -- no uncovered_branch markers. Branch coverage IS still measured
and reported (see coverage_gate.py's aggregate line/branch percentages,
still present in the gcovr --txt/--coveralls output), it is just not
per-marker ratcheted. This was a deliberate reversal after PR #850's CI run:
gcov's branch-edge ids (src/dst block ids) are NOT stable across gcc major
versions -- different majors split compound conditionals into a different
number of edges for the identical source line, so a baseline seeded under
one gcc major (e.g. a dev machine's Homebrew gcc-16) produces spurious
"new" markers under a different major (e.g. ubuntu-latest's stock gcc in
CI) for code nobody touched. There is no marker identity that survives
this -- not even a per-line branch *count*, since the split itself
differs. Line numbers do not have this problem (a source line is a source
line regardless of gcc major), so only lines are baselined. Fixing this
properly would mean pinning an exact gcc major across every dev machine
and CI, which is a bigger, separate fight (B1-642 already covers the
genuine-GNU-vs-Apple-clang trap; this is not that).

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
    marker set. LINES ONLY -- see module docstring for why branch edges are
    deliberately never baselined (not stable across gcc majors).

    A line gcovr itself marks `gcovr/excluded` (LCOV_EXCL_LINE) is skipped --
    it already does not count toward gcovr's own line_percent, so counting
    it here too would baseline a false gap gcovr never actually reports as
    uncovered."""
    markers: Set[Marker] = set()
    for f in detail.get("files", []):
        path = f["file"]
        for line in f.get("lines", []):
            if line.get("gcovr/excluded"):
                continue
            if line.get("count", 0) == 0:
                markers.add(Marker("uncovered_line", path, str(line["line_number"])))
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


def check(root: str, current: Set[Marker]) -> bool:
    """PASS/FAIL check for `make coverage`: FAIL iff there is any marker in
    `current` not already in the committed baseline."""
    baseline = load_baseline(root)
    new, removed = diff(current, baseline)

    for m in removed:
        print(f"INFO [coverage-baseline]: candidate to prune (now covered): "
              f"{m.path}:{m.id} ({m.type})")

    if new:
        by_file: dict = {}
        for m in new:
            by_file.setdefault(m.path, []).append(m)
        for path, ms in sorted(by_file.items()):
            ids = ", ".join(m.id for m in ms)
            print(f"ERROR [coverage-baseline]: {path}: {len(ms)} new uncovered "
                  f"line(s) not in baseline: {ids}", file=sys.stderr)
        print(
            f"coverage_baseline: {len(new)} new uncovered marker(s) -- FAIL "
            "(new/changed code must reach 100% LINE coverage; a file with "
            "pre-existing baseline gaps must have ALL of them closed once "
            "you touch it -- branch coverage is measured/reported above but "
            "NOT gated here, see scripts/coverage_baseline.py)",
            file=sys.stderr,
        )
        return False

    print(f"coverage_baseline: {len(current)} known-uncovered line marker(s) "
          "(baselined), 0 new -- PASS (branch coverage is measured/reported "
          "above but not ratcheted -- see scripts/coverage_baseline.py)")
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


def seed(root: str, current: Set[Marker]) -> Path:
    """One-time: bless the current uncovered set wholesale as the starting
    baseline. Errors (via caller) if a baseline already exists -- mirrors
    bbtool fence's --seed semantics."""
    return write_baseline(root, current)
