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
    remove+add."""
    cur_by_identity: Dict = {}
    for m in current:
        cur_by_identity.setdefault(identity_fn(m), m)
    base_by_identity: Dict = {}
    for m in baseline:
        base_by_identity.setdefault(identity_fn(m), m)

    new_ids = set(cur_by_identity) - set(base_by_identity)
    removed_ids = set(base_by_identity) - set(cur_by_identity)

    new = sorted((cur_by_identity[i] for i in new_ids), key=lambda m: (m.type, m.path, m.id))
    removed = sorted((base_by_identity[i] for i in removed_ids), key=lambda m: (m.type, m.path, m.id))
    return new, removed


def counts_by_bucket(markers: Set[Marker], bucket_fn: Callable[[str], str] = None) -> dict:
    out: dict = {}
    for m in markers:
        bucket = bucket_fn(m.type) if bucket_fn else m.type
        out[bucket] = out.get(bucket, 0) + 1
    return out
