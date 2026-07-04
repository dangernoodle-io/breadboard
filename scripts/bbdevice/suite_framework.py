"""Suite contract — generic device-suite helpers.

SettleConfig, SuiteContext, and gate_enabled relocated from fleet's
suites/__init__.py. Intentionally generic: the TM suite registry
(SUITES / load_suite) and mining-specific code stay behind in fleet.
"""
from __future__ import annotations
import copy
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, TYPE_CHECKING

if TYPE_CHECKING:
    from .device.results import ResultSet
    from .device.safety import Guard
    from .device import criteria_core
    from .device import profiles as profiles_mod
    from .device.readiness_core import ExtraCheck, Readiness


@dataclass
class SettleConfig:
    settle_delay: int = 0
    enabled: bool = False

    def wait_ready(
        self,
        device,
        criteria: Optional["criteria_core.Criteria"] = None,
        extra_checks: Sequence["ExtraCheck"] = (),
        timeout: int = 300,
    ) -> "Readiness":
        """Wait for `device` to become ready, wired to device.readiness_core.

        When disabled (or settle_delay <= 0), returns immediately as ready
        — mirrors the pre-restore no-op fallback callers relied on.
        """
        from .device import criteria_core as _criteria_core
        from .device import readiness_core

        if not self.enabled or self.settle_delay <= 0:
            return readiness_core.Readiness(ready=True, elapsed_s=0.0, reason="settle disabled")

        crit = criteria or _criteria_core.Criteria()
        if crit.settle_delay != self.settle_delay:
            crit = copy.copy(crit)
            crit.settle_delay = self.settle_delay
        return readiness_core.wait_until_ready(
            device, crit, timeout=timeout, extra_checks=extra_checks
        )


@dataclass
class SuiteContext:
    """Shared context passed to every suite's run() function."""
    devices: list                        # list[Device] — already resolved + filtered
    criteria: "criteria_core.Criteria"
    guard: "Guard"
    results: "ResultSet"
    fields: Optional[List[str]]          # on-demand field selection; None = all
    gates: set                           # enabled checks; empty = all
    settle: SettleConfig
    out_json: Optional[str]
    out_junit: Optional[str]
    baseline: Optional[str]
    profiles: Optional["profiles_mod.Profiles"] = None  # board-class overrides from profiles.yaml
    extra: dict = field(default_factory=dict)  # suite-specific parsed args


def gate_enabled(ctx: SuiteContext, name: str) -> bool:
    """Return True if the named gate/check is enabled."""
    return not ctx.gates or name in ctx.gates
