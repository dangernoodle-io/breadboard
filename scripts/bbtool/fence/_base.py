"""Shared engine for bbtool's `fence` command.

A "family" is a fence sub-scanner group: one module under `fence/` (this
package) exposing one or more `_scan_<name>(root) -> Set[Marker]`
functions, plus its own committed baseline at
`.baseline/bbtool/fence/<family>.json`. This module holds everything that
is generic across families — file walking, the `Marker` type, baseline
load/save, ratchet-diff, and scanner auto-discovery. It intentionally knows
nothing about any specific family's marker types or regexes; see
`fence/di_legacy.py` for a concrete family.
"""
from __future__ import annotations
import inspect
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Callable, Dict, Iterable, List, NamedTuple, Set

# Directory names skipped everywhere a family walks the tree (build
# artifacts, tooling caches, test fixtures — never source of truth for a
# fence marker).
EXCLUDE_DIRS = {".pio", ".claude", "build", "managed_components", "test", "tests"}


class Marker(NamedTuple):
    type: str
    path: str   # POSIX-style, relative to repo root
    id: str


# ---------------------------------------------------------------------------
# File walking helpers — shared by every family's `_scan_*` functions.
# ---------------------------------------------------------------------------

def iter_files(root: Path, scan_roots: Iterable[str], globs: Iterable[str]) -> List[Path]:
    out: List[Path] = []
    for scan_root_name in scan_roots:
        scan_root = root / scan_root_name
        if not scan_root.is_dir():
            continue
        for pattern in globs:
            for path in scan_root.glob(pattern):
                if not path.is_file():
                    continue
                parts = path.relative_to(root).parts
                if any(p in EXCLUDE_DIRS for p in parts):
                    continue
                out.append(path)
    return sorted(set(out))


def read(path: Path) -> str:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def rel(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def is_noise_line(stripped: str) -> bool:
    """Cheap (non-tokenizing) comment-line filter for C/C++ sources: skip
    lines that are entirely a // comment or a /* */ continuation (leading
    '*'). Deliberately conservative — over-matching (treating a
    commented-out reference as a real occurrence) is the safe failure
    direction for a ratchet fence."""
    return stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*")


def is_cmake_noise_line(stripped: str) -> bool:
    """CMakeLists comment-line filter: skip lines that are entirely a `#`
    comment."""
    return stripped.startswith("#")


# ---------------------------------------------------------------------------
# Scanner auto-discovery — the turnkey convention for adding a scanner to a
# family: define a module-level `_scan_<name>(root)` function; it is
# collected automatically, no manual registration list to edit.
# ---------------------------------------------------------------------------

def discover_scanners(module) -> List[Callable[[Path], Set[Marker]]]:
    scanners = []
    for name, fn in inspect.getmembers(module, inspect.isfunction):
        if not name.startswith("_scan_"):
            continue
        params = list(inspect.signature(fn).parameters.values())
        if len(params) != 1:
            continue
        scanners.append(fn)
    scanners.sort(key=lambda fn: fn.__name__)
    return scanners


class ScannerError(RuntimeError):
    """Raised when a discovered `_scan_*` function misbehaves — wraps the
    underlying exception (or the bad-return-value complaint) with a clear
    diagnostic naming the offending `module._scan_fn`, so a broken scanner
    never surfaces as an opaque TypeError/AttributeError deep in a set
    union."""


def scan_all(module, root: str) -> Set[Marker]:
    root_p = Path(root)
    found: Set[Marker] = set()
    for scanner in discover_scanners(module):
        qualname = f"{module.__name__}.{scanner.__name__}"
        try:
            result = scanner(root_p)
        except Exception as exc:  # noqa: BLE001 — re-raised with context below
            raise ScannerError(
                f"fence scanner {qualname}(root) raised {exc.__class__.__name__}: {exc}"
            ) from exc
        try:
            result_set = set(result)
        except TypeError as exc:
            raise ScannerError(
                f"fence scanner {qualname}(root) returned a non-iterable"
                f" ({result.__class__.__name__}) instead of Set[Marker]"
            ) from exc
        bad = [m for m in result_set if not isinstance(m, Marker)]
        if bad:
            raise ScannerError(
                f"fence scanner {qualname}(root) returned {len(bad)} non-Marker"
                f" element(s) (e.g. {bad[0]!r}) — every scanner must return"
                " Set[Marker]"
            )
        found |= result_set
    return found


# ---------------------------------------------------------------------------
# Identity — the ratchet-diff key. Families may override via a module-level
# `identity(marker) -> tuple` function (see fence/di_legacy.py for the
# path-insensitive pub_sink example); default is (type, id).
# ---------------------------------------------------------------------------

def default_identity(m: Marker):
    return (m.type, m.id)


def _sort_key(m: Marker):
    return (m.type, m.path, m.id)


def identity_fn_for(module) -> Callable[[Marker], tuple]:
    return getattr(module, "identity", default_identity)


# ---------------------------------------------------------------------------
# Baseline load/save — one JSON file per family, by convention at
# .baseline/bbtool/fence/<family>.json.
# ---------------------------------------------------------------------------

def baseline_path(root: str, family: str) -> Path:
    return Path(root) / ".baseline" / "bbtool" / "fence" / f"{family}.json"


def load_baseline(root: str, family: str) -> Set[Marker]:
    path = baseline_path(root, family)
    if not path.is_file():
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    return {Marker(e["type"], e["path"], e["id"]) for e in data.get("entries", [])}


def write_baseline(root: str, family: str, markers: Set[Marker]) -> Path:
    path = baseline_path(root, family)
    entries = [
        {"type": m.type, "path": m.path, "id": m.id}
        for m in sorted(markers, key=lambda m: (m.type, m.path, m.id))
    ]
    payload = {"entries": entries}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# Diff — rename-stable ratchet comparison keyed by identity_fn, not path.
# ---------------------------------------------------------------------------

def diff(current: Set[Marker], baseline: Set[Marker], identity_fn: Callable[[Marker], tuple] = default_identity):
    """Returns (new_markers, removed_markers) — both sorted lists.

    Comparison is by identity_fn(), not the full (type, path, id) tuple, so
    a pure file rename of an existing marker is never reported as a
    remove+add.

    Ratchets on the OCCURRENCE COUNT per identity, not just the identity's
    presence/absence (B1-917): identity is deliberately path-insensitive
    (that's what makes the fence rename-stable), but that means a SECOND
    occurrence reusing an already-baselined identity (e.g. a new file whose
    enclosing-symbol/var text happens to match a baselined one) used to
    collapse invisibly onto the existing entry — `set(cur) - set(base)` on
    identities alone is blind to a 1->2 count bump. Comparing occurrence
    counts per identity closes that hole while still treating a pure
    1-for-1 rename (count unchanged) as a no-op."""
    cur_by_identity: Dict[tuple, List[Marker]] = {}
    for m in current:
        cur_by_identity.setdefault(identity_fn(m), []).append(m)
    base_by_identity: Dict[tuple, List[Marker]] = {}
    for m in baseline:
        base_by_identity.setdefault(identity_fn(m), []).append(m)

    new: List[Marker] = []
    removed: List[Marker] = []
    for ident in set(cur_by_identity) | set(base_by_identity):
        cur_list = cur_by_identity.get(ident, [])
        base_list = base_by_identity.get(ident, [])

        # Pair markers by EXACT PATH first — a path present on both sides
        # is the same occurrence (unchanged), whether or not a rename
        # elsewhere in this identity group coincides with a genuine
        # add/remove. Only the leftovers below are new/removed candidates
        # (fixes the path-misattribution bug where a rename+add/remove
        # coincidence under one identity named the wrong file).
        base_path_counts = Counter(m.path for m in base_list)
        cur_leftover: List[Marker] = []
        for m in cur_list:
            if base_path_counts.get(m.path, 0) > 0:
                base_path_counts[m.path] -= 1
            else:
                cur_leftover.append(m)
        cur_path_counts = Counter(m.path for m in cur_list)
        base_leftover: List[Marker] = []
        for m in base_list:
            if cur_path_counts.get(m.path, 0) > 0:
                cur_path_counts[m.path] -= 1
            else:
                base_leftover.append(m)

        # Any leftovers remaining on BOTH sides are ambiguous renames
        # (path changed, but the count surplus/deficit doesn't require
        # them to be new/removed) — pair them off deterministically
        # (alphabetically) as unreported no-ops. Only the surplus beyond
        # that pairing is genuinely new/removed.
        paired = min(len(cur_leftover), len(base_leftover))
        new.extend(sorted(cur_leftover, key=_sort_key)[paired:])
        removed.extend(sorted(base_leftover, key=_sort_key)[paired:])

    new.sort(key=_sort_key)
    removed.sort(key=_sort_key)
    return new, removed


def counts_by_bucket(markers: Set[Marker], bucket_fn: Callable[[str], str] = None) -> dict:
    out: dict = {}
    for m in markers:
        bucket = bucket_fn(m.type) if bucket_fn else m.type
        out[bucket] = out.get(bucket, 0) + 1
    return out


# ---------------------------------------------------------------------------
# owner_of_path fallback observability — `clamp`/`sat_sub`/`callback_slot`'s
# `_component_of` falls back to a first-path-segment heuristic whenever
# `discovery.owner_of_path` returns None. Not reachable given the current
# tree layout, but not structurally impossible either (see clamp.py's
# `_component_of` docstring for the two known gap shapes: a file at zero
# nesting depth under `components/`, or a `platform/<variant>/...` file
# outside `discovery.PLATFORMS`). If it ever DID fire, that would mean the
# SSOT (`discovery.py`) and a family's own scan-root convention have drifted
# — a real problem nobody would otherwise learn about, since the fallback
# always returns a usable-looking string. This tracker + WARN line make that
# silent-drift scenario observable (fence_cmd.py folds the count into the
# per-family summary line) without making the fallback itself fatal — it
# stays a safety net, not a new failure mode.
# ---------------------------------------------------------------------------

_FALLBACK_COUNTS: Dict[str, int] = {}


def reset_owner_fallback_count(family: str) -> None:
    _FALLBACK_COUNTS[family] = 0


def owner_fallback_count(family: str) -> int:
    return _FALLBACK_COUNTS.get(family, 0)


def record_owner_fallback(family: str, rel_path: str) -> None:
    """Bump `family`'s fallback counter and emit a WARN to stderr naming the
    path `discovery.owner_of_path` couldn't resolve. Called from a family's
    `_component_of` on the None branch; never fatal."""
    _FALLBACK_COUNTS[family] = _FALLBACK_COUNTS.get(family, 0) + 1
    print(
        f"WARN [fence:{family}]: owner_of_path fallback fired for {rel_path}"
        " — discovery's SSOT and this family's scan-root convention"
        " disagree; falling back to the first path segment as owner",
        file=sys.stderr,
    )
