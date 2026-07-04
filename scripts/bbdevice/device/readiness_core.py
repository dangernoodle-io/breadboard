"""Generic post-reboot settle and readiness gate.

Derived from TaipanMiner fleetlib/readiness.py. Keeps only the heap +
reachability readiness path. Board-specific readiness branches from the
source module stay in fleet — a downstream consumer plugs them in here via
the `extra_checks` hook: each extra check is a callable taking the current
ReadinessSnapshot + Criteria and returning an optional failure reason
string (None = check passed).
"""
from __future__ import annotations
import logging
import time
from dataclasses import dataclass
from typing import Callable, List, Optional, Sequence, Tuple, TYPE_CHECKING

from .client import Client, TIMEOUT_INFO

if TYPE_CHECKING:
    from .criteria_core import Criteria

logger = logging.getLogger(__name__)


@dataclass
class Readiness:
    ready: bool
    elapsed_s: float
    reason: str


@dataclass
class ReadinessSnapshot:
    """Normalised view of one device poll used by the shared readiness predicate.

    Fields:
        heap_free: heap.internal.free (or free_heap flat fallback); None when unavailable.
    """
    heap_free: Optional[int]


# An extra check inspects the snapshot + criteria and returns a failure
# reason string, or None when it passes. A downstream consumer registers
# its own board-specific checks this way rather than bbdevice knowing
# about them.
ExtraCheck = Callable[[ReadinessSnapshot, "Criteria"], Optional[str]]


def is_ready(
    snapshot: ReadinessSnapshot,
    criteria: "Criteria",
    extra_checks: Sequence[ExtraCheck] = (),
) -> Tuple[bool, List[str]]:
    """Shared readiness predicate. Pure function — no I/O, no side-effects.

    Returns (ready: bool, reasons: list[str]). `reasons` is empty when ready.

    Logic:
    - heap_free >= criteria.readiness_heap_floor (required; None -> not ready)
    - each extra_checks callable runs in sequence; any returned reason is
      appended (this is where a downstream consumer's board-specific
      readiness branches plug in)
    """
    reasons: List[str] = []

    if snapshot.heap_free is None:
        reasons.append("heap_free unavailable")
    elif snapshot.heap_free < criteria.readiness_heap_floor:
        reasons.append(
            f"heap_free {snapshot.heap_free} < floor {criteria.readiness_heap_floor}"
        )

    for check in extra_checks:
        reason = check(snapshot, criteria)
        if reason:
            reasons.append(reason)

    return len(reasons) == 0, reasons


def wait_until_ready(
    client,
    criteria: "Criteria",
    timeout: int = 300,
    extra_checks: Sequence[ExtraCheck] = (),
) -> Readiness:
    """Poll until device is ready. Never returns before settle_delay has elapsed.

    Tolerates transient 5xx / connection refused (treats as not-ready, keeps polling).

    Args:
        client: bbdevice Client OR Device (duck-typed: needs get_json or .ip/.port)
        criteria: Criteria with settle_delay and readiness_heap_floor
        timeout: max seconds to wait before declaring not-ready
        extra_checks: additional readiness predicates (see ExtraCheck)

    Returns:
        Readiness(ready, elapsed_s, reason)
    """
    if hasattr(client, "get_json"):
        c = client
    else:
        c = Client(client.ip, getattr(client, "port", 80))

    settle = criteria.settle_delay
    t0 = time.monotonic()
    settle_deadline = t0 + settle
    poll_interval = 5  # seconds between polls

    last_reason = "polling not started"

    while True:
        elapsed = time.monotonic() - t0
        if elapsed >= timeout:
            return Readiness(ready=False, elapsed_s=elapsed, reason=last_reason)

        info = _safe_get(c, "/api/info", TIMEOUT_INFO)
        heap_free: Optional[int] = None

        if info is not None:
            heap = _safe_get(c, "/api/diag/heap", TIMEOUT_INFO)
            if heap is not None:
                heap_free = (heap.get("internal") or {}).get("free")
            if heap_free is None:
                heap_free = info.get("free_heap")

        snapshot = ReadinessSnapshot(heap_free=heap_free)

        ready_flag, reasons = is_ready(snapshot, criteria, extra_checks)

        elapsed = time.monotonic() - t0

        if ready_flag and elapsed >= settle:
            logger.debug(
                "device ready after %.1fs (settle_delay=%ds)", elapsed, settle
            )
            return Readiness(ready=True, elapsed_s=elapsed, reason="ready")

        if ready_flag:
            last_reason = f"waiting for settle_delay ({settle - elapsed:.0f}s remaining)"
        else:
            last_reason = "; ".join(reasons)
            logger.debug("not ready at %.1fs: %s", elapsed, last_reason)

        sleep_until = min(
            time.monotonic() + poll_interval,
            settle_deadline,
        )
        remaining = sleep_until - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)


def _safe_get(client, path: str, timeout: float) -> Optional[dict]:
    """GET path, returning None on any error (network, 5xx, parse)."""
    try:
        return client.get_json(path, timeout=timeout)
    except Exception:
        return None
