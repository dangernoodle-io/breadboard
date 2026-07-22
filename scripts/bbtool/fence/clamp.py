"""clamp fence family — ratchet-fence for hand-rolled reimplementations of
the already-extracted `bb_num` two-sided clamp helpers.

`bb_num` (components/bb_num) extracted a small, previously-duplicated idiom
into a portable, host-testable component: two-sided numeric clamping
(`bb_clampi`/`bb_clampf`). This fence freezes the CURRENT set of hand-rolled
reimplementations as a draining baseline: no *new* one may appear outside
the canonical component, but existing sites are grandfathered until
migrated onto `bb_num`.

One family per shared helper (see also `fence/scalar_parse.py`) — this is
the natural "family = module" use of the generic fence engine
(`fence/_base.py`); it makes per-helper lockdown/draindown and a fresh
`--seed` per helper clean, and keeps adding a future helper's fence (e.g. a
one-sided saturating-subtract idiom) a turnkey new module rather than a
combined-family rewrite.

Marker type scanned: `clamp` — a hand-rolled two-sided numeric clamp
(if-pair, nested ternary, or MIN/MAX nesting idiom) reimplementing
bb_clampi/bb_clampf. New composition never needs a new occurrence — use
`bb_num` directly instead of reimplementing.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path
from typing import List, Set

from discovery import build_index, ComponentIndex

from fence import _base
from fence._base import Marker

_SCAN_ROOTS = ("components", "platform")
_SRC_GLOBS = ["**/*.c", "**/*.h", "**/*.cpp"]

# ---------------------------------------------------------------------------
# Marker-type -> reporting bucket
# ---------------------------------------------------------------------------

_BUCKETS = {
    "clamp": "hand-rolled clamp",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# "owning component" name + best-effort enclosing symbol.
#
# Identity choice: a clamp marker's `id` is `<component>:<enclosing-symbol>:
# <var>`, NEVER `path:line` — a raw line number shifts on any unrelated edit
# above the clamp (e.g. adding a comment), which would spuriously turn a
# no-op edit into a remove+add under the default (type, id) ratchet-diff
# identity (see fence/_base.py:diff). Keying on the owning component plus
# the enclosing function name is the closest stable proxy for "this specific
# clamp site" without a real AST (bbtool is stdlib-only, no C parser
# available) — it survives line-shift and even a same-file reformat, and
# only collides if the *same* component/function pair hand-rolls two
# distinct clamps on a variable of the same name, which is rare and, per
# fence/_base.py's documented convention, collapsing that case into one
# ratchet entry is the safe (under-keying) failure direction for a ratchet
# fence, not a spurious-new failure.
# ---------------------------------------------------------------------------

_FAMILY = "clamp"


def _component_of(index: ComponentIndex, rel_path: str) -> str:
    """Owning component name for a marker path — delegates to the
    canonical `discovery.owner_of_path` SSOT (B1-1089; consolidated off
    this family's own hand-rolled `components/<name>/...` /
    `platform/<variant>/<name>/...` path-position match). On the `None`
    branch, delegates the fallback-vs-hard-fail decision to
    `_base.resolve_owner_fallback` (B1-1128; never a second hand-rolled
    component-like/loose-file distinction per family): it HARD-FAILS for a
    `components/<name>/...` (or `platform/<variant>/<name>/...`)
    directory discovery couldn't resolve — e.g. one missing a
    `CMakeLists.txt` anywhere on its branch, a real tree-integrity defect
    — and only falls back to the pre-consolidation first-path-segment
    heuristic (WARN + counter) for the two shapes that heuristic remains a
    safe stand-in for: (1) a genuine loose file directly at
    `components/<file>.c` or `platform/<plat>/<file>.c` (2-3 path parts,
    no intervening component-name directory); (2) a path outside the
    components/platform convention entirely (e.g. `examples/...`)."""
    name = index.owner_of_path(rel_path)
    if name is not None:
        return name
    return _base.resolve_owner_fallback(_FAMILY, rel_path)


_CONTROL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}
_ENCLOSING_FN_RE = re.compile(r'^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\(')

# K&R / split-return-type fallback: a bare `name(...)` signature line at
# column 0 with no return-type prefix on the same line (the return type
# sits on the PREVIOUS line instead, e.g. `int32_t\nbb_clampi(int32_t x,
# ...)`). `_ENCLOSING_FN_RE` requires a leading return-type token and so
# never matches this shape. Distinguishing a genuine bare signature line
# from a column-0 call/macro-invocation statement (e.g.
# `BB_INIT_REGISTER(x, y);`) is done the same way this codebase already
# writes such invocations: they always end in `;` on the invocation line,
# while a function definition's parameter-list line never does (its next
# non-blank content is `{`, not `;`). This is a heuristic, not a parse —
# see the module-level identity-choice comment above.
_BARE_SIG_RE = re.compile(r'^([A-Za-z_]\w*)\s*\(')


def _enclosing_symbol(lines: List[str], line_idx: int) -> str:
    """Best-effort enclosing function name for the marker at `line_idx`
    (0-based): walk backward to the nearest column-0 (non-indented) line
    that looks like a C function signature (`<ret-type> name(`, or the bare
    `name(` K&R/split-signature fallback above). Column-0 is a solid
    heuristic in this codebase's layout (top-level function defs are never
    indented; everything nested inside a body is) and is cheap (no real C
    parser — bbtool is stdlib-only). Falls back to "?" if none is found
    within the file (e.g. a macro body, or matched code above the first
    function)."""
    for i in range(line_idx, -1, -1):
        line = lines[i]
        if not line or line[0].isspace():
            continue
        m = _ENCLOSING_FN_RE.match(line)
        if m and m.group(1) not in _CONTROL_KEYWORDS:
            return m.group(1)
        m2 = _BARE_SIG_RE.match(line)
        if m2 and m2.group(1) not in _CONTROL_KEYWORDS and not line.rstrip().endswith(";"):
            return m2.group(1)
    return "?"


def _opposite_direction(op1: str, op2: str) -> bool:
    lt = {"<", "<="}
    gt = {">", ">="}
    return (op1 in lt and op2 in gt) or (op1 in gt and op2 in lt)


# ---------------------------------------------------------------------------
# clamp — hand-rolled two-sided numeric clamps.
#
# Three idiom variants are scanned, all requiring the SAME variable bounded
# from BOTH directions (opposite comparison operators) — this is what
# distinguishes a genuine two-sided clamp from a one-sided saturating op
# (e.g. bb_queue's underflow-clamp-at-0 `(*bytes_used >= len) ? (*bytes_used
# - len) : 0`, or bb_task_resolve's single-bound unicore-affinity fallback),
# which this scanner deliberately does NOT match — a one-sided saturate has
# no second, opposite-direction bound to reimplement bb_clampi/bb_clampf's
# actual job. (A future `sat_sub` family is the right home for fencing that
# one-sided idiom, not this one.)
#
#   (a) if-pair:   if (x < lo) x = lo;
#                  if (x > hi) x = hi;
#       (and >=/<= variants, either order) — both statements must be on
#       their own line (this codebase's prevailing style for this idiom;
#       a braced multi-line `if (...) { x = ...; }` variant is a known,
#       accepted gap — not seen in the tree today).
#   (b) nested ternary: x < lo ? lo : (x > hi ? hi : x)
#   (c) MIN/MAX nesting: MAX(lo, MIN(hi, x)) / MIN(hi, MAX(lo, x))
#       (and fmaxf/fminf, std::max/std::min spellings)
#
# The canonical implementation (platform/host/bb_num/) and its public
# header (components/bb_num/, which carries clamp-shaped doc-comment
# examples) are excluded, as are tests (excluded generically by
# fence/_base.py's EXCLUDE_DIRS). Matched by COMPONENT NAME via the
# discovery SSOT (index.owner_of_path), not a path-prefix tuple — a path
# prefix silently stops matching if bb_num ever relocates, which would make
# the canonical implementation trip its own fence (B1-1090 consumer
# migration); name-based matching survives a relocation since `bb_num` stays
# `bb_num` regardless of nesting depth.
# ---------------------------------------------------------------------------

_CANONICAL_CLAMP_COMPONENTS = ("bb_num",)

_IF_ASSIGN_RE = re.compile(
    r'^if\s*\(\s*(\w+)\s*(<=|>=|<|>)\s*([\w.]+)\s*\)\s*\1\s*=\s*[\w.]+\s*;\s*$'
)

_TERNARY_CLAMP_RE = re.compile(
    r'(\w[\w.\[\]\->]*)\s*(<=|>=|<|>)\s*([\w.]+)\s*\?\s*\3\s*:\s*\(\s*\1\s*(<=|>=|<|>)\s*([\w.]+)\s*\?\s*\5\s*:\s*\1\s*\)'
)

# MAX(lo, MIN(hi, x)) / fmaxf(lo, fminf(hi, x)) / std::max(lo, std::min(hi, x))
# and the swapped MIN(hi, MAX(lo, x)) nesting order.
_MINMAX_OUTER = r'(?:MAX|fmaxf|std::max)'
_MINMAX_INNER = r'(?:MIN|fminf|std::min)'
_MINMAX_CLAMP_RE = re.compile(
    r'\b' + _MINMAX_OUTER + r'\s*\(\s*[\w.]+\s*,\s*' + _MINMAX_INNER +
    r'\s*\(\s*[\w.]+\s*,\s*(\w[\w.\[\]\->]*)\s*\)\s*\)'
    r'|'
    r'\b' + _MINMAX_INNER + r'\s*\(\s*[\w.]+\s*,\s*' + _MINMAX_OUTER +
    r'\s*\(\s*[\w.]+\s*,\s*(\w[\w.\[\]\->]*)\s*\)\s*\)'
)


def _scan_if_pair_clamps(index: ComponentIndex, rel: str, lines: List[str]) -> Set[Marker]:
    found: Set[Marker] = set()
    for i in range(len(lines) - 1):
        l1 = lines[i].strip()
        if not l1 or _base.is_noise_line(l1):
            continue
        m1 = _IF_ASSIGN_RE.match(l1)
        if not m1:
            continue
        l2 = lines[i + 1].strip()
        if not l2 or _base.is_noise_line(l2):
            continue
        m2 = _IF_ASSIGN_RE.match(l2)
        if not m2:
            continue
        var1, op1 = m1.group(1), m1.group(2)
        var2, op2 = m2.group(1), m2.group(2)
        if var1 != var2 or not _opposite_direction(op1, op2):
            continue
        component = _component_of(index, rel)
        symbol = _enclosing_symbol(lines, i)
        found.add(Marker("clamp", rel, f"{component}:{symbol}:{var1}"))
    return found


def _scan_ternary_clamps(index: ComponentIndex, rel: str, text: str, lines: List[str]) -> Set[Marker]:
    found: Set[Marker] = set()
    for m in _TERNARY_CLAMP_RE.finditer(text):
        op1, op2 = m.group(2), m.group(4)
        if not _opposite_direction(op1, op2):
            continue
        line_idx = text.count("\n", 0, m.start())
        if _base.is_noise_line(lines[line_idx].strip()):
            continue
        var = m.group(1)
        component = _component_of(index, rel)
        symbol = _enclosing_symbol(lines, line_idx)
        found.add(Marker("clamp", rel, f"{component}:{symbol}:{var}"))
    return found


def _scan_minmax_clamps(index: ComponentIndex, rel: str, text: str, lines: List[str]) -> Set[Marker]:
    found: Set[Marker] = set()
    for m in _MINMAX_CLAMP_RE.finditer(text):
        var = m.group(1) or m.group(2)
        line_idx = text.count("\n", 0, m.start())
        if _base.is_noise_line(lines[line_idx].strip()):
            continue
        component = _component_of(index, rel)
        symbol = _enclosing_symbol(lines, line_idx)
        found.add(Marker("clamp", rel, f"{component}:{symbol}:{var}"))
    return found


def _scan_clamp(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    index = build_index([str(root)])
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        if index.owner_of_path(rel) in _CANONICAL_CLAMP_COMPONENTS:
            continue
        text = _base.read(path)
        lines = text.splitlines()
        found |= _scan_if_pair_clamps(index, rel, lines)
        found |= _scan_ternary_clamps(index, rel, text, lines)
        found |= _scan_minmax_clamps(index, rel, text, lines)
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
