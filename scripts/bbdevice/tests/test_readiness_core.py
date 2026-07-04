"""Tests for bbdevice.device.readiness_core — offline unit tests with mocked clients.

Ported (generic subset) from TaipanMiner scripts/fleet/tests/test_readiness.py —
mining/pool/hashrate/vcore branches stay in fleet; this file covers the
heap + reachability readiness path plus the extra_checks extension point.
"""
from __future__ import annotations
import time
import unittest
from unittest.mock import MagicMock, patch

from bbdevice.device.criteria_core import Criteria
from bbdevice.device.readiness_core import (
    wait_until_ready,
    is_ready,
    ReadinessSnapshot,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_client(info=None, heap=None, raises=None):
    """Build a mock Client whose get_json returns canned responses per path."""
    c = MagicMock()

    def _get_json(path, timeout=5):
        if raises:
            raise raises
        mapping = {
            "/api/info": info,
            "/api/diag/heap": heap,
        }
        return mapping.get(path)

    c.get_json = _get_json
    return c


def _ready_info():
    return {"uptime_ms": 30000, "board": "esp32-wroom32", "version": "v0.69.0"}


def _ready_heap():
    return {"internal": {"free": 80000, "min_free": 75000}}


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestFloorElapsed(unittest.TestCase):
    """wait_until_ready must never return before settle_delay has elapsed."""

    def test_floor_elapsed_small_settle(self):
        c = _make_client(info=_ready_info(), heap=_ready_heap())
        criteria = Criteria(settle_delay=2, readiness_heap_floor=50_000)
        r = wait_until_ready(c, criteria, timeout=30)
        self.assertTrue(r.ready, f"expected ready=True, reason={r.reason!r}")
        self.assertGreaterEqual(
            r.elapsed_s, 2,
            f"elapsed {r.elapsed_s:.2f}s < settle_delay 2s",
        )

    def test_floor_elapsed_zero_settle(self):
        c = _make_client(info=_ready_info(), heap=_ready_heap())
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
        t0 = time.monotonic()
        r = wait_until_ready(c, criteria, timeout=30)
        elapsed = time.monotonic() - t0
        self.assertTrue(r.ready)
        self.assertLess(elapsed, 5.0)


class TestTimeout(unittest.TestCase):
    """wait_until_ready must return Readiness(ready=False) when timeout expires."""

    def test_timeout_heap_never_meets_floor(self):
        c = _make_client(
            info=_ready_info(),
            heap={"internal": {"free": 10000}},  # always below 50k floor
        )
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
        t0 = time.monotonic()
        r = wait_until_ready(c, criteria, timeout=8)
        elapsed = time.monotonic() - t0
        self.assertFalse(r.ready, f"expected ready=False, got {r}")
        self.assertGreaterEqual(elapsed, 7)

    def test_timeout_device_unreachable(self):
        c = _make_client()  # all endpoints return None
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
        r = wait_until_ready(c, criteria, timeout=8)
        self.assertFalse(r.ready)


class TestTransientErrors(unittest.TestCase):
    """wait_until_ready must tolerate transient errors and eventually succeed."""

    def test_transient_connection_errors(self):
        call_count = {"n": 0}
        c = MagicMock()

        def _get_json(path, timeout=5):
            if path == "/api/info":
                call_count["n"] += 1
                if call_count["n"] <= 3:
                    raise ConnectionError("transient failure")
                return _ready_info()
            if path == "/api/diag/heap":
                if call_count["n"] <= 3:
                    return None
                return _ready_heap()
            return None

        c.get_json = _get_json
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
        r = wait_until_ready(c, criteria, timeout=60)
        self.assertTrue(r.ready, f"expected ready=True after transient errors, reason={r.reason!r}")
        self.assertGreater(call_count["n"], 3, "expected at least 4 calls")

    def test_osError_treated_as_not_ready(self):
        call_count = {"n": 0}
        c = MagicMock()

        def _get_json(path, timeout=5):
            call_count["n"] += 1
            if call_count["n"] < 3:
                raise OSError("connection refused")
            if path == "/api/info":
                return _ready_info()
            if path == "/api/diag/heap":
                return _ready_heap()
            return None

        c.get_json = _get_json
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
        r = wait_until_ready(c, criteria, timeout=60)
        self.assertTrue(r.ready)


class TestDeviceLike(unittest.TestCase):
    """wait_until_ready accepts a Device-like object (has .ip, .port) in addition to Client."""

    def test_device_object(self):
        class FakeDevice:
            ip = "127.0.0.1"
            port = 80

        with patch("bbdevice.device.readiness_core.Client") as MockClient:
            instance = MagicMock()

            def _get_json(path, timeout=5):
                if path == "/api/info":
                    return _ready_info()
                if path == "/api/diag/heap":
                    return _ready_heap()
                return None

            instance.get_json = _get_json
            MockClient.return_value = instance

            criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
            r = wait_until_ready(FakeDevice(), criteria, timeout=30)
            self.assertTrue(r.ready)
            MockClient.assert_called_once_with("127.0.0.1", 80)


class TestIsReadyPredicate(unittest.TestCase):
    """Unit tests for the shared is_ready(snapshot, criteria) predicate."""

    def _criteria(self, **kw) -> Criteria:
        defaults = dict(settle_delay=0, readiness_heap_floor=50_000)
        defaults.update(kw)
        return Criteria(**defaults)

    def test_heap_below_floor_not_ready(self):
        snap = ReadinessSnapshot(heap_free=10_000)
        ready, reasons = is_ready(snap, self._criteria(readiness_heap_floor=50_000))
        self.assertFalse(ready)
        self.assertTrue(any("heap_free" in r for r in reasons), reasons)

    def test_heap_at_floor_ready(self):
        snap = ReadinessSnapshot(heap_free=50_000)
        ready, reasons = is_ready(snap, self._criteria(readiness_heap_floor=50_000))
        self.assertTrue(ready, reasons)

    def test_heap_above_floor_ready(self):
        snap = ReadinessSnapshot(heap_free=80_000)
        ready, reasons = is_ready(snap, self._criteria(readiness_heap_floor=50_000))
        self.assertTrue(ready, reasons)

    def test_heap_none_not_ready(self):
        snap = ReadinessSnapshot(heap_free=None)
        ready, reasons = is_ready(snap, self._criteria())
        self.assertFalse(ready)
        self.assertTrue(any("unavailable" in r for r in reasons), reasons)

    def test_reasons_empty_when_ready(self):
        snap = ReadinessSnapshot(heap_free=80_000)
        ready, reasons = is_ready(snap, self._criteria())
        self.assertTrue(ready)
        self.assertEqual(reasons, [])


class TestExtraChecksExtensionPoint(unittest.TestCase):
    """extra_checks hook — is_ready() runs each callable in sequence."""

    def test_passing_extra_check_does_not_block_readiness(self):
        snap = ReadinessSnapshot(heap_free=80_000)
        check = lambda s, c: None
        ready, reasons = is_ready(snap, Criteria(readiness_heap_floor=50_000), extra_checks=[check])
        self.assertTrue(ready)
        self.assertEqual(reasons, [])

    def test_failing_extra_check_blocks_readiness(self):
        snap = ReadinessSnapshot(heap_free=80_000)
        check = lambda s, c: "mining hashrate below floor"
        ready, reasons = is_ready(snap, Criteria(readiness_heap_floor=50_000), extra_checks=[check])
        self.assertFalse(ready)
        self.assertIn("mining hashrate below floor", reasons)

    def test_multiple_extra_checks_run_in_sequence(self):
        snap = ReadinessSnapshot(heap_free=80_000)
        calls = []

        def check_a(s, c):
            calls.append("a")
            return None

        def check_b(s, c):
            calls.append("b")
            return "b failed"

        ready, reasons = is_ready(
            snap, Criteria(readiness_heap_floor=50_000), extra_checks=[check_a, check_b]
        )
        self.assertEqual(calls, ["a", "b"])
        self.assertFalse(ready)
        self.assertEqual(reasons, ["b failed"])

    def test_wait_until_ready_passes_extra_checks_through(self):
        """wait_until_ready threads extra_checks into is_ready via the poll loop."""
        c = _make_client(info=_ready_info(), heap=_ready_heap())
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000)
        failing_check = lambda s, cr: "extension not ready"
        r = wait_until_ready(c, criteria, timeout=3, extra_checks=[failing_check])
        self.assertFalse(r.ready)
        self.assertIn("extension not ready", r.reason)


if __name__ == "__main__":
    unittest.main()
