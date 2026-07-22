"""bbtool fence families — auto-discovered scanner+baseline groups.

Adding a new family is turnkey: drop a module in this package (any file
whose name does not start with `_`) exposing one or more
`_scan_<name>(root) -> Set[Marker]` functions (see
`_base.discover_scanners`). It is auto-imported and auto-registered in
`FAMILIES` below by module name — no manual registry-list edit required —
and gets its own baseline at `.baseline/bbtool/fence/<module-name>.json` by
the same convention. Optionally define a module-level
`identity(marker) -> tuple` to override the default `(type, id)`
ratchet-diff identity key.

See `fence/di_legacy.py` for a concrete family and `fence/_base.py` for the
generic engine (file walking, Marker type, baseline load/save, diff).
"""
from __future__ import annotations
import importlib
import pkgutil
from typing import Dict

from . import _base  # noqa: F401
from ._base import (  # noqa: F401
    Marker,
    ScannerError,
    UnresolvedComponentOwnerError,
    baseline_path,
    counts_by_bucket,
    default_identity,
    diff,
    discover_scanners,
    identity_fn_for,
    is_component_like_gap,
    load_baseline,
    owner_fallback_count,
    record_owner_fallback,
    reset_owner_fallback_count,
    resolve_owner_fallback,
    scan_all,
    write_baseline,
)

FAMILIES: Dict[str, object] = {}


def _discover() -> None:
    if FAMILIES:
        return
    for info in pkgutil.iter_modules(__path__):
        if info.name.startswith("_"):
            continue
        FAMILIES[info.name] = importlib.import_module(f"{__name__}.{info.name}")


_discover()
