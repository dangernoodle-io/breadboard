"""Suite contract — generic device-suite helpers.

SettleConfig, SuiteContext, and gate_enabled relocated from fleet's
suites/__init__.py. Intentionally generic: the TM suite registry
(SUITES / load_suite) and mining-specific code stay behind in fleet.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from .device.results import ResultSet
    from .device.safety import Guard


@dataclass
class SettleConfig:
    settle_delay: int = 0
    enabled: bool = False


@dataclass
class SuiteContext:
    """Shared context passed to every suite's run() function."""
    devices: list                        # list[Device] — already resolved + filtered
    criteria: object
    guard: "Guard"
    results: "ResultSet"
    fields: Optional[List[str]]          # on-demand field selection; None = all
    gates: set                           # enabled checks; empty = all
    settle: SettleConfig
    out_json: Optional[str]
    out_junit: Optional[str]
    baseline: Optional[str]
    profiles: Optional[object] = None           # board-class overrides from profiles.yaml
    extra: dict = field(default_factory=dict)  # suite-specific parsed args


def gate_enabled(ctx: SuiteContext, name: str) -> bool:
    """Return True if the named gate/check is enabled."""
    return not ctx.gates or name in ctx.gates
