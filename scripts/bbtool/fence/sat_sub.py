"""sat_sub fence family — ratchet-fence for hand-rolled saturating-subtract
idioms (a one-sided subtract floored at 0 to avoid unsigned/negative
underflow, e.g. `used = (total > free) ? (total - free) : 0`).

There is no canonical extracted helper for this idiom yet (unlike
`bb_num`/`bb_scalar` for `clamp`/`scalar_parse` — see B3, parked). This
fence exists to STOP the hand-rolled duplication from growing further while
extraction is deferred: it freezes the CURRENT set of sites as a draining
baseline, same shrink-only ratchet semantics as every other family.

One family per shared idiom (see also `fence/clamp.py`, `fence/scalar_parse
.py`) — this is the natural "family = module" use of the generic fence
engine (`fence/_base.py`), the turnkey new-module case `clamp.py`'s
module docstring anticipated.

Marker types scanned:
  - `sat_sub` — a one-sided saturating subtract, either:
      - the ternary/guard-expression shape: `(A > B) ? (A - B) : 0` (or
        `>=`, or the reversed `A < B ? 0 : A - B` / `<=`), including the
        decrement-by-1 special case `(A > 0) ? A - 1 : 0`; or
      - the post-hoc delta-then-clamp shape: `X = <expr containing a
        subtraction>; if (X < 0) X = 0;` (single-line or braced form) —
        see `_scan_sat_sub_delta` below.

Deliberately NOT matched (out of scope for this family):
  - two-sided clamps (both directions bounded) — that is `fence/clamp.py`'s
    job, not this one; `clamp.py` explicitly carves out the one-sided
    saturate as sat_sub's future home, and this module is that home.
  - bounded-chunk loops (`remaining -= chunk` where `chunk` was already
    computed as `min(remaining, N)`) — there is no separate underflow
    guard being reimplemented there, just an ordinary decrement of an
    already-bounded quantity.
  - a bare `if (n < 0) return ...;` / `if (n < 0) ...;` error-code guard —
    the delta-then-clamp scanner requires the IMMEDIATELY preceding
    (non-blank, non-comment) line to be an assignment to that same
    variable whose right-hand side contains a subtraction; an unrelated
    negative-value guard whose prior line isn't such an assignment never
    matches.
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
    "sat_sub": "hand-rolled saturating-subtract",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# "owning component" + best-effort enclosing symbol — same conventions as
# clamp.py (see that module's identity-choice comment for the full
# rationale: `<component>:<enclosing-symbol>:<var>`, never `path:line`, so
# an unrelated edit above a site never trips the fence).
# ---------------------------------------------------------------------------

_CONTROL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}
_ENCLOSING_FN_RE = re.compile(r'^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\(')
_BARE_SIG_RE = re.compile(r'^([A-Za-z_]\w*)\s*\(')


_FAMILY = "sat_sub"


def _component_of(index: ComponentIndex, rel_path: str) -> str:
    """Owning component name for a marker path — delegates to the
    canonical `discovery.owner_of_path` SSOT (B1-1089; see `clamp.py`'s
    twin helper for the full rationale, identical here). On the `None`
    branch, delegates the fallback-vs-hard-fail decision to
    `_base.resolve_owner_fallback` (B1-1128; never a second hand-rolled
    component-like/loose-file distinction per family) — see `clamp.py`'s
    twin helper for the full rationale, identical here."""
    name = index.owner_of_path(rel_path)
    if name is not None:
        return name
    return _base.resolve_owner_fallback(_FAMILY, rel_path)


def _enclosing_symbol(lines: List[str], line_idx: int) -> str:
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


# ---------------------------------------------------------------------------
# sat_sub — hand-rolled one-sided saturating-subtract idioms.
#
# Two ternary shapes, both requiring the SAME variable/expression on both
# sides of the subtraction (`A`) and the SAME bound (`B`) in the guard and
# the subtraction — this is what distinguishes a genuine saturating
# subtract from an unrelated ternary that merely happens to have a `- ` and
# a `: 0` somewhere in it:
#
#   (a) forward:  (A > B) ? (A - B) : 0     / (A >= B) ? (A - B) : 0
#   (b) reversed: (A < B) ? 0 : (A - B)     / (A <= B) ? 0 : (A - B)
#
# `B` may be an identifier/member-access expression OR a numeric literal
# (covers bb_mdns's `(out_size > 6) ? (int)(out_size - 6) : 0`, a literal
# bound). An optional cast (`(size_t)`, `(uint32_t)`, `(int)`, ...) is
# allowed immediately before the parenthesized subtraction, and the
# subtraction's own parens are optional.
#
# A THIRD, narrower shape covers the decrement-by-1 special case flagged by
# the duplication sweep: `(A > 0) ? A - 1 : 0` — the guard bound is the
# literal 0 but the amount subtracted is the literal 1, so it does not fit
# the "same B on both sides" shape above; matched explicitly instead of
# generalizing the main regex (which would loosen it enough to catch
# unrelated ternaries whose true-branch subtracts an unrelated constant).
# ---------------------------------------------------------------------------

_CAST_OPT = r'(?:\(\s*[A-Za-z_][\w ]*\)\s*)?'

_SATSUB_FWD_RE = re.compile(
    r'(\*?[\w.\[\]]+(?:->[\w.\[\]]+)*)\s*(>=|>)\s*([\w.\[\]]+(?:->[\w.\[\]]+)*|\d+)\s*'
    r'\)?\s*\?\s*' + _CAST_OPT + r'\(?\s*\1\s*-\s*\3\s*\)?\s*'
    r':\s*0[UuLl]*\b'
)

_SATSUB_REV_RE = re.compile(
    r'(\*?[\w.\[\]]+(?:->[\w.\[\]]+)*)\s*(<=|<)\s*([\w.\[\]]+(?:->[\w.\[\]]+)*|\d+)\s*'
    r'\)?\s*\?\s*0[UuLl]*\s*:\s*' + _CAST_OPT + r'\(?\s*\1\s*-\s*\3\s*\)?\b'
)

_SATSUB_DEC1_RE = re.compile(
    r'(\*?[\w.\[\]]+(?:->[\w.\[\]]+)*)\s*>\s*0\s*'
    r'\)?\s*\?\s*' + _CAST_OPT + r'\(?\s*\1\s*-\s*1\s*\)?\s*'
    r':\s*0[UuLl]*\b'
)


def _is_noise_line(stripped: str) -> bool:
    """Like `_base.is_noise_line`, but corrected for a false-positive that
    matters specifically for this family: `_base.is_noise_line` treats any
    stripped line starting with `*` as a `/* ... */` continuation (its
    generic doxygen-comment heuristic), which also matches an ordinary
    pointer-dereference STATEMENT such as `*bytes_used = (*bytes_used >=
    len) ? (*bytes_used - len) : 0;` (bb_queue's site, this codebase's
    prevailing no-space `*name` deref style) — sat_sub's canonical
    saturating-subtract sites live disproportionately on exactly this
    shape (in/out pointer-parameter update), so the generic filter would
    silently blind this family to real sites. Distinguish by what follows
    the `*`: a genuine comment continuation is always `* text`, `*/`, or
    `**...` (space/slash/asterisk immediately after), never a bare
    identifier character."""
    if stripped.startswith("//") or stripped.startswith("/*"):
        return True
    if stripped.startswith("*"):
        rest = stripped[1:]
        return not rest or rest[0] in " \t*/"
    return False


def _add_ternary_matches(index: ComponentIndex, found: Set[Marker], rel: str, text: str,
                          lines: List[str], pattern: re.Pattern) -> None:
    for m in pattern.finditer(text):
        line_idx = text.count("\n", 0, m.start())
        if _is_noise_line(lines[line_idx].strip()):
            continue
        var = m.group(1)
        component = _component_of(index, rel)
        symbol = _enclosing_symbol(lines, line_idx)
        found.add(Marker("sat_sub", rel, f"{component}:{symbol}:{var}"))


# No canonical sat_sub helper exists yet (extraction is B3, parked — see
# module docstring), but `bb_num` is excluded anyway, same as clamp.py: its
# header/impl can carry doc-comment examples contrasting a two-sided clamp
# against a one-sided saturate, and that reference must never self-fire any
# fence family. Matched by COMPONENT NAME via the discovery SSOT
# (index.owner_of_path), not a path-prefix tuple — a path prefix silently
# stops matching if bb_num ever relocates.
_SAT_SUB_EXCLUDE_COMPONENTS = ("bb_num",)


def _scan_sat_sub(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    index = build_index([str(root)])
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        if index.owner_of_path(rel) in _SAT_SUB_EXCLUDE_COMPONENTS:
            continue
        text = _base.read(path)
        lines = text.splitlines()
        _add_ternary_matches(index, found, rel, text, lines, _SATSUB_FWD_RE)
        _add_ternary_matches(index, found, rel, text, lines, _SATSUB_REV_RE)
        _add_ternary_matches(index, found, rel, text, lines, _SATSUB_DEC1_RE)
    return found


# ---------------------------------------------------------------------------
# sat_sub (delta-then-clamp variant) — the post-hoc "compute a delta via
# subtraction, then floor a non-monotonic-clock/negative result at 0"
# idiom, e.g.:
#
#   int64_t elapsed_us = now_us - last_log_us;
#   if (elapsed_us < 0) {
#       elapsed_us = 0;
#   }
#
# or the single-line form:
#
#   int payload_len = (int)(entry_len - topic_len - 1);
#   if (payload_len < 0) payload_len = 0;
#
# PRECISION GUARD: this only fires when the line immediately preceding the
# `if (VAR < 0)` guard (skipping blank/comment lines) is itself an
# assignment TO THAT SAME VAR whose right-hand side contains a subtraction.
# This is what distinguishes the idiom from the far more common bare
# negative-error-code guard (`if (n < 0) return false;`, `if (idx < 0) {
# ... }`, etc.) scattered throughout the tree — those never have a
# subtraction-to-the-same-var assignment on the immediately preceding line,
# so they never match.
# ---------------------------------------------------------------------------

# Optional leading type-declaration tokens (`int64_t`, `size_t`, `int
# payload_len`, ...) before the assigned variable — declaration+init and a
# plain reassignment must both be caught.
_DELTA_ASSIGN_RE = re.compile(r'^(?:[A-Za-z_]\w*[\s*]+)*(\w+)\s*=\s*(.+);$')
# RHS must contain a genuine binary subtraction (identifier/close-bracket
# on the left of `-`, identifier/open-paren on the right) — excludes a
# lone unary-negative literal RHS like `x = -1;`.
_DELTA_SUB_IN_RHS_RE = re.compile(r'[\w)\]]\s*-\s*[\w(]')


def _delta_guard_matches(lines: List[str], guard_idx: int, var: str) -> bool:
    """True if the (already located) `if (var < 0) ...` guard line at
    `guard_idx` completes the delta-then-clamp idiom: either the
    single-line `if (var < 0) var = 0;` form, or the braced
    `if (var < 0) {` form whose next non-blank/non-comment body line is
    `var = 0;`."""
    guard = lines[guard_idx].strip()
    var_re = re.escape(var)
    single = re.match(
        r'^if\s*\(\s*' + var_re + r'\s*<\s*0\s*\)\s*' + var_re + r'\s*=\s*0\s*;?\s*$',
        guard,
    )
    if single:
        return True
    brace = re.match(r'^if\s*\(\s*' + var_re + r'\s*<\s*0\s*\)\s*\{\s*$', guard)
    if not brace:
        return False
    k = guard_idx + 1
    n = len(lines)
    while k < n and (not lines[k].strip() or _is_noise_line(lines[k].strip())):
        k += 1
    if k >= n:
        return False
    return bool(re.match(r'^' + var_re + r'\s*=\s*0\s*;', lines[k].strip()))


def _scan_sat_sub_delta(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    index = build_index([str(root)])
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        if index.owner_of_path(rel) in _SAT_SUB_EXCLUDE_COMPONENTS:
            continue
        text = _base.read(path)
        lines = text.splitlines()
        n = len(lines)
        for i in range(n):
            stripped = lines[i].strip()
            if not stripped or _is_noise_line(stripped):
                continue
            m = _DELTA_ASSIGN_RE.match(stripped)
            if not m or not _DELTA_SUB_IN_RHS_RE.search(m.group(2)):
                continue
            var = m.group(1)
            j = i + 1
            while j < n and (not lines[j].strip() or _is_noise_line(lines[j].strip())):
                j += 1
            if j >= n or not _delta_guard_matches(lines, j, var):
                continue
            component = _component_of(index, rel)
            symbol = _enclosing_symbol(lines, i)
            found.add(Marker("sat_sub", rel, f"{component}:{symbol}:{var}"))
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
