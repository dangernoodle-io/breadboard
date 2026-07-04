"""Generic device poll engine with pluggable detector framework.

Derived from TaipanMiner fleetlib/monitor.py. Keeps the poll engine and the
GENERIC detectors (heap floor, heap leak, reboot, reset-reason, wdt,
downtime). Board-specific detector factories from the source module stay
behind in TaipanMiner's fleet harness.

Two extension points:
  - `_sample_device` takes a configurable list of (field_name, path)
    endpoints instead of hardcoding a fixed set — the generic caller
    passes bb-owned endpoints; other consumers pass their own endpoint
    set separately.
  - `register_detector(name, factory)` backs a module-level detector
    registry so a consumer can register additional detector factories at
    import time and have them assembled by name via
    detectors_from_criteria(extra_detector_names=...).

Usage:
    detectors = detectors_from_criteria(criteria)
    anomalies = poll(devices, interval=60, duration=3600, detectors=detectors)
    if anomalies:
        sys.exit(1)
"""
from __future__ import annotations
import logging
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple, TYPE_CHECKING

if TYPE_CHECKING:
    from .criteria_core import Criteria
    from .discovery import Device

logger = logging.getLogger(__name__)


def _device_is_ready(sample: "Sample", criteria: "Criteria") -> bool:
    """Evaluate the shared readiness predicate against a Sample (heap-only).

    Generic warmup gate: heap floor + reachability. Board-specific readiness
    signals are not evaluated here — a downstream poll wrapper (not yet
    moved) layers those in via readiness_core's extra_checks hook.
    """
    from .readiness_core import is_ready, ReadinessSnapshot

    heap_free: Optional[int] = None
    if sample.heap is not None:
        heap_free = (sample.heap.get("internal") or {}).get("free")
    if heap_free is None and sample.info is not None:
        heap_free = sample.info.get("free_heap")

    snap = ReadinessSnapshot(heap_free=heap_free)
    ready, _ = is_ready(snap, criteria)
    return ready


@dataclass
class Sample:
    device: "Device"
    timestamp: float
    info: Optional[dict]
    heap: Optional[dict]
    telemetry: Optional[dict]
    ok: bool  # False when /api/info was unreachable
    warmup: bool = False


@dataclass
class Anomaly:
    device: "Device"
    detector: str   # detector name (e.g. "heap_floor", "reboot")
    message: str
    sample: Sample


# Detector signature: (sample, per-device-state-dict) -> Anomaly | None
Detector = Callable[[Sample, Dict[str, Any]], Optional[Anomaly]]

# endpoint = (field_name, path); caller supplies which endpoints to sample.
Endpoint = Tuple[str, str]

_DEFAULT_ENDPOINTS: Tuple[Endpoint, ...] = (
    ("info", "/api/info"),
    ("heap", "/api/diag/heap"),
    ("telemetry", "/api/telemetry"),
)


def _sample_device(device: "Device", endpoints: Optional[Sequence[Endpoint]] = None) -> Sample:
    """Sample a device over a configurable list of (field_name, path) endpoints.

    The generic caller passes bb-owned endpoints (info/heap/telemetry); a
    board-specific caller (not in this PR) passes additional endpoints and
    reads them out of a richer Sample subclass/wrapper on its side.
    """
    from .client import Client, TIMEOUT_INFO, TIMEOUT_TELEMETRY
    if endpoints is None:
        endpoints = _DEFAULT_ENDPOINTS
    c = Client(device.ip, device.port)
    fetched: Dict[str, Optional[dict]] = {}
    for field_name, path in endpoints:
        timeout = TIMEOUT_TELEMETRY if field_name == "telemetry" else TIMEOUT_INFO
        fetched[field_name] = c.get_json(path, timeout=timeout)
    return Sample(
        device=device,
        timestamp=time.time(),
        info=fetched.get("info"),
        heap=fetched.get("heap"),
        telemetry=fetched.get("telemetry"),
        ok=fetched.get("info") is not None,
    )


def poll(
    devices: List["Device"],
    interval: float,
    duration: float,
    detectors: List[Detector],
    endpoints: Optional[Sequence[Endpoint]] = None,
    settle_delay: int = 0,
    on_sample: Optional[Callable[[Sample], None]] = None,
    criteria: Optional["Criteria"] = None,
) -> List[Anomaly]:
    """Poll all devices for `duration` seconds at `interval` second ticks.

    Each tick: sample each device then run all detectors against the sample.
    Anomalies are accumulated; polling continues to completion (does not stop
    on first anomaly — callers decide what to do with the list).

    Args:
        devices:      list of Device to monitor
        interval:     seconds between poll ticks (including sampling time)
        duration:     total seconds to run
        detectors:    list of Detector callables
        endpoints:    subset of (field_name, path) tuples to sample
                      (default: info/heap/telemetry)
        settle_delay: warmup floor in seconds after start (and after reboots).
                      When criteria is supplied, warmup ends when the shared
                      readiness predicate passes AND settle_delay has elapsed.
                      When criteria is None, warmup ends when settle_delay elapses
                      (legacy blind-timer behavior).
                      0 = no warmup suppression (default, preserves old behavior).
        on_sample:    optional callback invoked for every sample (including warmup ticks).
                      sample.warmup is set before the callback fires. Detectors still
                      observe warmup suppression regardless of this callback.
        criteria:     Criteria instance used to evaluate the shared readiness predicate
                      during warmup. When None (or settle_delay == 0), warmup is purely
                      time-based (backward-compatible).

    Returns:
        List of Anomaly detected across the run (may be empty).
    """
    if endpoints is None:
        endpoints = _DEFAULT_ENDPOINTS
    eps = tuple(endpoints)

    now = time.monotonic()

    # Per-device state dict persists across ticks for stateful detectors
    state: Dict[str, Dict[str, Any]] = {d.ip: {} for d in devices}

    # Per-device warmup floor deadline (monotonic clock); key = device.ip.
    warmup_floor: Dict[str, float] = {
        d.ip: now + settle_delay for d in devices
    }
    # Per-device last-seen uptime to detect mid-run reboots
    last_uptime: Dict[str, Optional[float]] = {d.ip: None for d in devices}

    anomalies: List[Anomaly] = []
    t0 = time.time()

    while time.time() - t0 < duration:
        tick_start = time.time()
        for device in devices:
            sample = _sample_device(device, eps)
            st = state[device.ip]

            # Uptime regression = reboot mid-run; re-arm warmup window
            if settle_delay > 0 and sample.ok and sample.info is not None:
                cur_uptime = sample.info.get("uptime_ms")
                prev = last_uptime[device.ip]
                if (
                    cur_uptime is not None
                    and prev is not None
                    and cur_uptime < prev
                ):
                    new_floor = time.monotonic() + settle_delay
                    logger.info(
                        "reboot detected on %s (uptime %s -> %s ms); "
                        "resetting warmup window for %ds",
                        device.ip, prev, cur_uptime, settle_delay,
                    )
                    warmup_floor[device.ip] = new_floor
                if cur_uptime is not None:
                    last_uptime[device.ip] = cur_uptime

            # Warmup ends when:
            #   (a) settle_delay == 0  ->  never in warmup
            #   (b) floor has elapsed AND (criteria is None OR device is ready)
            if settle_delay > 0:
                floor_elapsed = time.monotonic() >= warmup_floor[device.ip]
                if floor_elapsed and criteria is not None and sample.ok:
                    in_warmup = not _device_is_ready(sample, criteria)
                else:
                    in_warmup = not floor_elapsed
            else:
                in_warmup = False

            sample.warmup = in_warmup
            if on_sample is not None:
                try:
                    on_sample(sample)
                except Exception as exc:
                    logger.error("on_sample callback error on %s: %s", device.ip, exc)

            for det in detectors:
                try:
                    a = det(sample, st)
                    if a is not None:
                        if in_warmup:
                            logger.debug(
                                "WARMUP-SUPPRESS [%s] %s (%s): %s",
                                a.detector, device.ip, device.board, a.message,
                            )
                        else:
                            logger.warning(
                                "ANOMALY [%s] %s (%s): %s",
                                a.detector, device.ip, device.board, a.message,
                            )
                            anomalies.append(a)
                except Exception as exc:
                    logger.error("detector error on %s: %s", device.ip, exc)

        elapsed = time.time() - tick_start
        sleep_time = max(0.0, interval - elapsed)
        if sleep_time > 0:
            time.sleep(sleep_time)

    return anomalies


# ---------------------------------------------------------------------------
# Detector factories (generic)
# ---------------------------------------------------------------------------

def make_heap_floor_detector(criteria: "Criteria") -> Detector:
    """Anomaly when heap.internal.free drops below the floor."""
    def _detect(sample: Sample, state: Dict) -> Optional[Anomaly]:
        if not sample.ok:
            return None
        heap = sample.heap or {}
        free = (heap.get("internal") or {}).get("free")
        # flat-field fallback for firmware older than B1-310
        if free is None and sample.info:
            free = sample.info.get("free_heap")
        if free is not None and free < criteria.heap_floor:
            return Anomaly(
                sample.device, "heap_floor",
                f"heap.internal.free={free} < floor {criteria.heap_floor}", sample,
            )
        return None
    return _detect


def make_heap_leak_detector(criteria: "Criteria") -> Detector:
    """Anomaly when min_free_ever declines (memory is being leaked)."""
    def _detect(sample: Sample, state: Dict) -> Optional[Anomaly]:
        if not sample.ok or not criteria.heap_leak_check:
            return None
        heap = sample.heap or {}
        min_free = (heap.get("internal") or {}).get("min_free")
        if min_free is None:
            return None
        prev = state.get("min_free_ever")
        if prev is not None and min_free < prev:
            return Anomaly(
                sample.device, "heap_leak",
                f"min_free declined: {min_free} < prev {prev}", sample,
            )
        # only update the watermark; never allow it to climb (tracks the true minimum)
        state["min_free_ever"] = min_free if prev is None else min(prev, min_free)
        return None
    return _detect


def make_reboot_detector(criteria: "Criteria") -> Detector:
    """Anomaly when uptime_ms regresses by more than reboot_tolerance_ms."""
    def _detect(sample: Sample, state: Dict) -> Optional[Anomaly]:
        if not sample.ok or sample.info is None:
            return None
        uptime = sample.info.get("uptime_ms", 0)
        max_up = state.get("max_uptime_ms", 0)
        if uptime + criteria.reboot_tolerance_ms < max_up:
            return Anomaly(
                sample.device, "reboot",
                f"uptime {uptime}ms < seen {max_up}ms "
                f"(regression > {criteria.reboot_tolerance_ms}ms)", sample,
            )
        state["max_uptime_ms"] = max(max_up, uptime)
        return None
    return _detect


def make_reset_reason_detector(criteria: "Criteria") -> Detector:
    """Anomaly when a bad reset_reason appears on a freshly-booted device."""
    def _detect(sample: Sample, state: Dict) -> Optional[Anomaly]:
        if not sample.ok or sample.info is None:
            return None
        rr = sample.info.get("reset_reason")
        uptime = sample.info.get("uptime_ms", 0)
        if rr in criteria.bad_reset_reasons and uptime < 150_000:
            return Anomaly(
                sample.device, "reset_reason",
                f"reset_reason={rr!r} with uptime {uptime}ms", sample,
            )
        return None
    return _detect


def make_wdt_detector(criteria: "Criteria") -> Detector:
    """Anomaly when wdt_resets count increases between polls."""
    def _detect(sample: Sample, state: Dict) -> Optional[Anomaly]:
        if not criteria.wdt_resets_flat or not sample.ok or sample.info is None:
            return None
        wdt = sample.info.get("wdt_resets")
        if wdt is None:
            return None
        prev = state.get("wdt_resets")
        if prev is not None and wdt > prev:
            return Anomaly(
                sample.device, "wdt_increase",
                f"wdt_resets increased: {prev} -> {wdt}", sample,
            )
        state["wdt_resets"] = wdt
        return None
    return _detect


def make_downtime_detector(criteria: "Criteria") -> Detector:
    """Anomaly when a device misses too many consecutive polls."""
    def _detect(sample: Sample, state: Dict) -> Optional[Anomaly]:
        if not sample.ok:
            miss = state.get("missed_polls", 0) + 1
            state["missed_polls"] = miss
            if miss >= criteria.max_missed_polls:
                return Anomaly(
                    sample.device, "downtime",
                    f"{miss} consecutive missed polls", sample,
                )
        else:
            state["missed_polls"] = 0
        return None
    return _detect


# ---------------------------------------------------------------------------
# Extension point: register_detector — a consumer registers additional
# detector factories under a name at import time; detectors_from_criteria()
# assembles them by name alongside the generic set when extra_detector_names
# is supplied.
# ---------------------------------------------------------------------------

DetectorFactory = Callable[["Criteria"], Detector]

_detector_registry: Dict[str, DetectorFactory] = {}


def register_detector(name: str, factory: DetectorFactory) -> None:
    """Register a named detector factory (Criteria) -> Detector.

    A downstream consumer calls this at import time to register its own
    board-specific detector factories without bbdevice knowing their names
    or implementations.
    """
    _detector_registry[name] = factory


def registered_detector_names() -> List[str]:
    """Return the names of all currently-registered extension detectors."""
    return list(_detector_registry.keys())


def detectors_from_criteria(
    criteria: "Criteria",
    extra_detector_names: Optional[Sequence[str]] = None,
) -> List[Detector]:
    """Build the standard generic detector list from a Criteria.

    Always included: downtime, reboot, reset_reason, wdt, heap_floor,
    heap_leak.

    extra_detector_names: names previously registered via register_detector()
    — each is instantiated with `criteria` and appended. Unknown names are
    silently skipped (registry not populated
    means the extension is simply absent for this run).
    """
    dets: List[Detector] = [
        make_downtime_detector(criteria),
        make_reboot_detector(criteria),
        make_reset_reason_detector(criteria),
        make_wdt_detector(criteria),
        make_heap_floor_detector(criteria),
        make_heap_leak_detector(criteria),
    ]
    for name in (extra_detector_names or ()):
        factory = _detector_registry.get(name)
        if factory is not None:
            dets.append(factory(criteria))
    return dets
