"""Canonical generic soak criteria — harness defaults, overridable per board profile.

Derived from TaipanMiner fleetlib/criteria.py. Keeps only the loader and
generic fields. Board-specific criteria fields from the source module are
not first-class attributes here — a downstream consumer reads them via the
`overlay` dict, which the loader stashes unknown YAML keys into. A single
dataclass with an overlay dict is required (not subclassing) because
dataclasses.replace() is used across the boundary elsewhere.
"""
from __future__ import annotations
import copy
import os
from dataclasses import dataclass, field
from typing import Any, Dict, Set, TYPE_CHECKING

if TYPE_CHECKING:
    from .profiles import Profile


@dataclass
class Criteria:
    """Canonical soak pass/fail thresholds (generic subset).

    All values are self-contained defaults; no external file is required.
    Load a YAML overlay with load(); apply board-class overrides with
    for_profile(). Unknown/extension YAML keys (board-specific fields owned
    by a downstream consumer) land in `overlay` rather than being dropped.
    """
    poll_interval: float = 60.0          # seconds between samples
    duration: float = 3600.0             # total soak window (seconds)
    heap_floor: int = 50_000             # bytes — heap.internal.free must stay >= this
    heap_leak_check: bool = True         # min_free must not decline over the soak window
    reboot_tolerance_ms: int = 30_000    # uptime regression > this => reboot detected
    bad_reset_reasons: Set[str] = field(
        default_factory=lambda: {"panic", "task_wdt", "int_wdt", "brownout"}
    )
    wdt_resets_flat: bool = True         # wdt_resets count must not increase
    version_check: bool = False          # require running version == target version
    max_missed_polls: int = 4            # consecutive missed polls => downtime anomaly
    # settle / readiness gate
    settle_delay: int = 120             # minimum warmup floor in seconds
    readiness_heap_floor: int = 50_000  # heap_internal.free must reach this before ready
    # extension point: unknown YAML keys (board-specific fields owned by a
    # downstream consumer) land here, e.g. criteria.overlay.get("some_floor_mv", 500).
    overlay: Dict[str, Any] = field(default_factory=dict)


_KNOWN_FIELDS = {
    "poll_interval", "duration", "heap_floor", "heap_leak_check",
    "reboot_tolerance_ms", "bad_reset_reasons", "wdt_resets_flat",
    "version_check", "max_missed_polls",
    "settle_delay", "readiness_heap_floor",
}


def load(path: str = "config/criteria.yaml") -> Criteria:
    """Load criteria from a YAML file, merging over defaults.

    Silently returns defaults when:
    - the file does not exist
    - pyyaml is not installed
    - the file is malformed

    Unknown keys (not a generic Criteria field) are stashed into `overlay`
    rather than dropped, so downstream extension consumers can still read
    them.
    """
    defaults = Criteria()
    if not os.path.exists(path):
        return defaults
    try:
        import yaml  # type: ignore[import]
    except ImportError:
        return defaults
    try:
        with open(path) as f:
            data = yaml.safe_load(f) or {}
    except Exception:
        return defaults

    for k, v in data.items():
        if k in _KNOWN_FIELDS:
            if k == "bad_reset_reasons":
                setattr(defaults, k, set(v))
            else:
                setattr(defaults, k, v)
        elif k != "overlay":
            defaults.overlay[k] = v
    return defaults


def for_profile(criteria: Criteria, profile: "Profile") -> Criteria:
    """Return a shallow copy of criteria with board-class overrides applied from profile.

    Only Profile fields that are not None override the corresponding Criteria field.
    bad_reset_reasons and overlay are copied so mutations don't affect the original.
    """
    c = copy.copy(criteria)
    c.bad_reset_reasons = set(criteria.bad_reset_reasons)  # deep copy the set
    c.overlay = dict(criteria.overlay)  # deep copy the overlay dict

    if profile.poll_interval is not None:
        c.poll_interval = profile.poll_interval
    if profile.heap_floor is not None:
        c.heap_floor = profile.heap_floor
    if profile.readiness_heap_floor is not None:
        c.readiness_heap_floor = profile.readiness_heap_floor
    return c
