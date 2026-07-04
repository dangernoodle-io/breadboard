"""Generic board-class capability profiles.

Derived from TaipanMiner fleetlib/profiles.py. Keeps only the generic
container types and a no-op default resolver. The source module's
board-taxonomy resolver (which detects board classes and applies
class-specific defaults) stays in fleet — it is not a bbdevice concern.
"""
from __future__ import annotations
import dataclasses
import os
from dataclasses import dataclass
from typing import Optional


@dataclass
class Profile:
    """Capability profile for a board class.

    Criteria overrides (None = use Criteria default):
      poll_interval, heap_floor, readiness_heap_floor
    """
    board: str
    has_psram: bool = False
    max_concurrent: int = 4   # max concurrent HTTP connections during stress
    max_rps: float = 10.0     # max requests/second during stress
    # httpd worker constraint: True for single-core, heap-tight boards
    single_worker: bool = False
    # criteria overrides
    poll_interval: Optional[float] = None   # seconds; None = use Criteria default
    heap_floor: Optional[int] = None        # bytes; None = use Criteria default
    readiness_heap_floor: Optional[int] = None  # bytes; None = use Criteria default


class Profiles:
    """Named board-class profile overrides loaded from a YAML file.

    Keys are board-class prefix strings matched by the caller's own
    resolver (e.g. default_profile below, or a TM-side profile_for()).
    Use load() to read the YAML; use get() to look up a class by name.
    """

    def __init__(self, overrides: Optional[dict] = None) -> None:
        self.overrides: dict = overrides or {}

    @classmethod
    def load(cls, path: str = "config/profiles.yaml") -> "Profiles":
        """Load profile overrides from YAML. Returns empty Profiles if file absent."""
        if not os.path.exists(path):
            return cls()
        try:
            import yaml  # type: ignore[import]
        except ImportError:
            return cls()
        try:
            with open(path) as f:
                data = yaml.safe_load(f) or {}
        except Exception:
            return cls()
        return cls(overrides=data)

    def get(self, board_class: str) -> Optional[Profile]:
        """Return a Profile for the given board_class key, or None if not defined."""
        entry = self.overrides.get(board_class)
        if entry is None:
            return None
        valid = {f.name for f in dataclasses.fields(Profile)}
        kw = {k: v for k, v in entry.items() if k in valid and k != "board"}
        return Profile(board=board_class, **kw)

    def __repr__(self) -> str:
        return f"Profiles({list(self.overrides.keys())})"


def default_profile(board: str, profiles: Optional["Profiles"] = None) -> Profile:
    """Identity/default resolver — no board-taxonomy matching.

    Checks *profiles* (a Profiles loaded from YAML) for an exact or
    prefix match first; falls back to a bare Profile with no overrides.
    Consumers needing board-class detection supply their own resolver —
    that logic is not generic.
    """
    if profiles is not None:
        b_lower = board.lower()
        for key in profiles.overrides:
            if b_lower == key or b_lower.startswith(key):
                override = profiles.get(key)
                if override is not None:
                    return override
    return Profile(board=board)
