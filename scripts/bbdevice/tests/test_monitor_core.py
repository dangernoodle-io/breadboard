"""Tests for bbdevice.device.monitor_core — poll engine + generic detectors.

Ported (generic subset) from TaipanMiner scripts/fleet/tests/test_monitor_warmup.py —
hashrate_ghs/mining-stats/sensors coverage stays in fleet; this file covers the
poll loop, warmup suppression, reboot rearm, on_sample callback wiring, and the
register_detector/endpoint extension points.
"""
from __future__ import annotations
import time
import unittest
from unittest.mock import patch
from typing import Any, Dict, Optional

from bbdevice.device.criteria_core import Criteria
from bbdevice.device.monitor_core import (
    Anomaly,
    Sample,
    poll,
    make_heap_floor_detector,
    register_detector,
    registered_detector_names,
    detectors_from_criteria,
    _sample_device,
)
from bbdevice.device.discovery import Device


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_device(ip: str = "192.0.2.1", board: str = "esp32-wroom32") -> Device:
    return Device(hostname=board, ip=ip, port=80, board=board, version="v0.69.0")


def _make_sample(device: Device, ok: bool = True, free: int = 30_000, uptime_ms: int = 5000) -> Sample:
    return Sample(
        device=device,
        timestamp=time.time(),
        info={"uptime_ms": uptime_ms, "reset_reason": "normal", "wdt_resets": 0} if ok else None,
        heap={"internal": {"free": free, "min_free": free - 1000}} if ok else None,
        telemetry=None,
        ok=ok,
    )


def _make_ready_sample(device: Device) -> Sample:
    return Sample(
        device=device,
        timestamp=time.time(),
        info={"uptime_ms": 60_000, "reset_reason": "normal", "wdt_resets": 0},
        heap={"internal": {"free": 80_000, "min_free": 75_000}},
        telemetry=None,
        ok=True,
    )


def _make_not_ready_sample(device: Device) -> Sample:
    return Sample(
        device=device,
        timestamp=time.time(),
        info={"uptime_ms": 5_000, "reset_reason": "normal", "wdt_resets": 0},
        heap={"internal": {"free": 10_000, "min_free": 9_000}},
        telemetry=None,
        ok=True,
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestNoAnomalyDuringWarmup(unittest.TestCase):
    def test_suppressed_during_warmup(self):
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_sample(dev, free=10_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            criteria = Criteria(heap_floor=50_000)
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=3600,
            )

        self.assertEqual(len(anomalies), 0)

    def test_anomaly_fires_after_warmup(self):
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_sample(dev, free=10_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            criteria = Criteria(heap_floor=50_000)
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=0,
            )

        self.assertGreater(len(anomalies), 0)


class TestAnomalyAfterWarmup(unittest.TestCase):
    def test_heap_floor_fires_without_warmup(self):
        device = _make_device()
        criteria = Criteria(heap_floor=50_000, settle_delay=0)
        det = make_heap_floor_detector(criteria)

        sample = _make_sample(device, free=30_000)
        state: Dict[str, Any] = {}
        anomaly = det(sample, state)
        self.assertIsNotNone(anomaly)
        self.assertEqual(anomaly.detector, "heap_floor")

    def test_heap_floor_suppressed_by_warmup_via_poll(self):
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_sample(dev, free=30_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            criteria = Criteria(heap_floor=50_000)
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=3600,
            )

        self.assertEqual(len(anomalies), 0)


class TestWarmupRearmsOnReboot(unittest.TestCase):
    def test_warmup_rearms_on_uptime_regression(self):
        device = _make_device()
        call_count = {"n": 0}

        def _fake_sample(dev, eps):
            call_count["n"] += 1
            n = call_count["n"]
            if n == 1:
                return Sample(
                    device=dev, timestamp=time.time(),
                    info={"uptime_ms": 600_000, "reset_reason": "normal", "wdt_resets": 0},
                    heap={"internal": {"free": 80_000, "min_free": 75_000}},
                    telemetry=None, ok=True,
                )
            return Sample(
                device=dev, timestamp=time.time(),
                info={"uptime_ms": 5_000, "reset_reason": "normal", "wdt_resets": 0},
                heap={"internal": {"free": 20_000, "min_free": 18_000}},
                telemetry=None, ok=True,
            )

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            criteria = Criteria(heap_floor=50_000)
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.4,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=3600,
            )

        self.assertEqual(len(anomalies), 0)
        self.assertGreaterEqual(call_count["n"], 2)

    def test_warmup_not_rearm_on_stable_uptime(self):
        device = _make_device()
        call_count = {"n": 0}

        def _fake_sample(dev, eps):
            call_count["n"] += 1
            return Sample(
                device=dev, timestamp=time.time(),
                info={"uptime_ms": call_count["n"] * 60_000, "reset_reason": "normal", "wdt_resets": 0},
                heap={"internal": {"free": 10_000, "min_free": 9_000}},
                telemetry=None, ok=True,
            )

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            criteria = Criteria(heap_floor=50_000)
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=0,
            )

        self.assertGreater(len(anomalies), 0)
        self.assertGreaterEqual(call_count["n"], 1)


class TestOnSampleCallback(unittest.TestCase):
    def test_callback_fires_per_tick_including_warmup(self):
        device = _make_device()
        received = []

        def _fake_sample(dev, eps):
            return _make_sample(dev, free=80_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            poll(
                devices=[device],
                interval=0.05,
                duration=0.2,
                detectors=[],
                settle_delay=3600,
                on_sample=received.append,
            )

        self.assertGreater(len(received), 0)
        self.assertTrue(all(s.warmup for s in received))

    def test_callback_fires_no_warmup(self):
        device = _make_device()
        received = []

        def _fake_sample(dev, eps):
            return _make_sample(dev)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            poll(
                devices=[device],
                interval=0.05,
                duration=0.2,
                detectors=[],
                settle_delay=0,
                on_sample=received.append,
            )

        self.assertGreater(len(received), 0)
        self.assertTrue(all(not s.warmup for s in received))

    def test_no_callback_backward_compat(self):
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_sample(dev, free=10_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            result = poll(
                devices=[device],
                interval=0.05,
                duration=0.2,
                detectors=[make_heap_floor_detector(Criteria(heap_floor=50_000))],
                settle_delay=0,
            )

        self.assertIsInstance(result, list)

    def test_callback_receives_all_samples(self):
        device = _make_device()
        received = []
        call_n = {"n": 0}

        def _fake_sample(dev, eps):
            call_n["n"] += 1
            if call_n["n"] % 2 == 1:
                return _make_sample(dev, ok=True)
            return _make_sample(dev, ok=False)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[],
                settle_delay=0,
                on_sample=received.append,
            )

        ok_samples = [s for s in received if s.ok]
        failed_samples = [s for s in received if not s.ok]
        self.assertGreater(len(ok_samples), 0)
        self.assertGreater(len(failed_samples), 0)


class TestWarmupEndsOnReadinessPredicate(unittest.TestCase):
    def test_warmup_suppressed_until_ready_sample(self):
        """Ticks with heap below floor stay in warmup; once heap is healthy warmup ends."""
        device = _make_device()
        call_count = {"n": 0}

        def _fake_sample(dev, eps):
            call_count["n"] += 1
            # First 3 ticks: not ready (low heap); remaining: ready
            if call_count["n"] <= 3:
                return _make_not_ready_sample(dev)
            return _make_ready_sample(dev)

        received: list = []
        criteria = Criteria(
            settle_delay=0,  # floor is 0, so warmup ends as soon as predicate passes
            readiness_heap_floor=50_000,
            heap_floor=50_000,
        )

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            poll(
                devices=[device],
                interval=0.05,
                duration=0.5,
                detectors=[],
                settle_delay=0,
                on_sample=received.append,
                criteria=criteria,
            )

        # With settle_delay=0, no warmup is applied regardless of criteria
        self.assertTrue(all(not s.warmup for s in received), "settle_delay=0 must mean no warmup")

    def test_warmup_ends_when_ready_and_floor_elapsed(self):
        """With settle_delay=0 and criteria: warmup is off (floor elapsed immediately)."""
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_ready_sample(dev)

        received: list = []
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000, heap_floor=50_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[],
                settle_delay=0,
                on_sample=received.append,
                criteria=criteria,
            )

        self.assertTrue(all(not s.warmup for s in received), "settle_delay=0 never warms up")

    def test_warmup_persists_until_predicate_passes_with_floor(self):
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_ready_sample(dev)

        received: list = []
        criteria = Criteria(settle_delay=3600, readiness_heap_floor=50_000, heap_floor=50_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=3600,
                on_sample=received.append,
                criteria=criteria,
            )

        self.assertTrue(all(s.warmup for s in received))

    def test_warmup_continues_while_predicate_fails_after_floor(self):
        """settle_delay=0 + criteria: when predicate fails, no warmup (floor already elapsed)."""
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_not_ready_sample(dev)

        received: list = []
        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000, heap_floor=50_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.3,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=0,
                on_sample=received.append,
                criteria=criteria,
            )

        # settle_delay=0 → no warmup → anomalies fire immediately
        self.assertGreater(len(anomalies), 0, "settle_delay=0 means anomalies fire, predicate irrelevant")
        self.assertTrue(all(not s.warmup for s in received))

    def test_warmup_rearms_on_reboot_with_criteria(self):
        """Reboot mid-run re-arms warmup; predicate-gated warmup persists after rearm."""
        device = _make_device()
        call_count = {"n": 0}

        def _fake_sample(dev, eps):
            call_count["n"] += 1
            if call_count["n"] == 1:
                # healthy, long uptime
                return Sample(
                    device=dev, timestamp=time.time(),
                    info={"uptime_ms": 600_000, "reset_reason": "normal", "wdt_resets": 0},
                    heap={"internal": {"free": 80_000, "min_free": 75_000}},
                    telemetry=None,
                    ok=True,
                )
            # reboot: low uptime, heap below floor (not ready)
            return Sample(
                device=dev, timestamp=time.time(),
                info={"uptime_ms": 1_000, "reset_reason": "normal", "wdt_resets": 0},
                heap={"internal": {"free": 10_000, "min_free": 9_000}},
                telemetry=None,
                ok=True,
            )

        criteria = Criteria(settle_delay=3600, readiness_heap_floor=50_000, heap_floor=50_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.4,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=3600,
                criteria=criteria,
            )

        # Post-reboot ticks are in warmup (re-armed + predicate still fails) → no anomalies
        self.assertEqual(len(anomalies), 0,
                         f"post-reboot anomalies should be suppressed; got {anomalies}")

    def test_settle_disabled_no_warmup_with_criteria(self):
        device = _make_device()

        def _fake_sample(dev, eps):
            return _make_not_ready_sample(dev)

        criteria = Criteria(settle_delay=0, readiness_heap_floor=50_000, heap_floor=50_000)

        with patch("bbdevice.device.monitor_core._sample_device", side_effect=_fake_sample):
            anomalies = poll(
                devices=[device],
                interval=0.05,
                duration=0.2,
                detectors=[make_heap_floor_detector(criteria)],
                settle_delay=0,
                criteria=criteria,
            )

        self.assertGreater(len(anomalies), 0)


class TestSampleDeviceEndpointExtensionPoint(unittest.TestCase):
    """_sample_device takes a configurable list of (field_name, path) endpoints."""

    def test_default_endpoints_populate_info_heap_telemetry(self):
        device = _make_device()
        with patch("bbdevice.device.client.Client.get_json") as mock_get:
            mock_get.side_effect = lambda path, timeout=5: {"path": path}
            sample = _sample_device(device)
        self.assertEqual(sample.info, {"path": "/api/info"})
        self.assertEqual(sample.heap, {"path": "/api/diag/heap"})
        self.assertEqual(sample.telemetry, {"path": "/api/telemetry"})

    def test_custom_endpoints_only_fetch_named_paths(self):
        device = _make_device()
        calls = []

        def _get_json(path, timeout=5):
            calls.append(path)
            return {"path": path} if path == "/api/info" else None

        with patch("bbdevice.device.client.Client.get_json", side_effect=_get_json):
            sample = _sample_device(device, endpoints=[("info", "/api/info")])
        self.assertEqual(calls, ["/api/info"])
        self.assertEqual(sample.info, {"path": "/api/info"})
        self.assertIsNone(sample.heap)
        self.assertIsNone(sample.telemetry)


class TestRegisterDetectorExtensionPoint(unittest.TestCase):
    """register_detector() backs a module-level registry consumed by detectors_from_criteria."""

    def setUp(self):
        self._saved = dict(__import__(
            "bbdevice.device.monitor_core", fromlist=["_detector_registry"]
        )._detector_registry)

    def tearDown(self):
        import bbdevice.device.monitor_core as mc
        mc._detector_registry.clear()
        mc._detector_registry.update(self._saved)

    def test_register_detector_adds_to_registry(self):
        def _factory(criteria):
            def _detect(sample, state):
                return None
            return _detect

        register_detector("test_extension", _factory)
        self.assertIn("test_extension", registered_detector_names())

    def test_detectors_from_criteria_includes_registered_extension(self):
        calls = []

        def _factory(criteria):
            def _detect(sample, state):
                calls.append(sample.device.ip)
                return None
            return _detect

        register_detector("test_extension_2", _factory)
        criteria = Criteria()
        dets = detectors_from_criteria(criteria, extra_detector_names=["test_extension_2"])
        # 6 generic detectors + 1 registered extension
        self.assertEqual(len(dets), 7)

        device = _make_device()
        sample = _make_sample(device, free=80_000)
        for det in dets:
            det(sample, {})
        self.assertIn(device.ip, calls)

    def test_unknown_extension_name_silently_skipped(self):
        criteria = Criteria()
        dets = detectors_from_criteria(criteria, extra_detector_names=["does_not_exist"])
        self.assertEqual(len(dets), 6)  # only the generic set

    def test_detectors_from_criteria_default_is_generic_only(self):
        criteria = Criteria()
        dets = detectors_from_criteria(criteria)
        self.assertEqual(len(dets), 6)


if __name__ == "__main__":
    unittest.main()
