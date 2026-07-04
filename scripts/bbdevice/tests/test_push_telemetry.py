"""Offline tests for ResultSet.push_telemetry (TA-455).

No real broker is touched — paho client is injected via _client_factory.

Ported from TaipanMiner scripts/fleet/tests/test_push_telemetry.py — the
TestFleetCmdSuiteMetricsFlag class (commands._suite / suites / fleetlib.safety
/ fleetlib.criteria / fleetlib.profiles integration) stays behind; out of
scope for this device-layer move. Uses a local _Device stand-in in place of
fleetlib.discovery.Device (see test_results.py).
"""
from __future__ import annotations
import json
import unittest
from dataclasses import dataclass
from unittest.mock import MagicMock

from bbdevice.device.results import Result, ResultSet, STATUS_FAIL, STATUS_PASS, STATUS_SKIP


@dataclass
class _Device:
    hostname: str
    ip: str
    port: int
    board: str
    version: str


def _dev(ip: str = "192.0.2.1", board: str = "test-board") -> _Device:
    return _Device(hostname="test-host", ip=ip, port=80, board=board, version="v1.0.0")


def _make_published_factory():
    """Return (factory, published_list) where published_list accumulates publish() calls."""
    published = []

    class FakeClient:
        def __init__(self):
            self.on_connect = None
            self.on_publish = None

        def connect(self, host, port, keepalive=10):
            if self.on_connect:
                self.on_connect(self, None, None, 0)

        def publish(self, topic, payload, qos=1):
            published.append({"topic": topic, "payload": payload})
            info = MagicMock()
            info.mid = 1
            if self.on_publish:
                self.on_publish(self, None, 1)
            return info

        def disconnect(self):
            pass

        def loop_start(self):
            pass

        def loop_stop(self):
            pass

    return FakeClient, published


class TestPushTelemetryPublishes(unittest.TestCase):
    """push_telemetry connects and publishes one message per result with metrics."""

    def setUp(self):
        self.factory, self.published = _make_published_factory()

    def test_publishes_one_message_per_result_with_metrics(self):
        rs = ResultSet("soak")
        rs.add(Result("r1", _dev(), STATUS_PASS, "", {"heap_free_min": 60000}))
        rs.add(Result("r2", _dev("192.0.2.2", "bitaxe-601"), STATUS_PASS, "",
                      {"hashrate_avg": 485.0}))
        rs.push_telemetry("broker.example:1883", _client_factory=self.factory)
        self.assertEqual(len(self.published), 2)

    def test_topic_format(self):
        rs = ResultSet("stress")
        rs.add(Result("r", _dev(board="esp32-wroom32"), STATUS_PASS, "",
                      {"duration_s": 30.0}))
        rs.push_telemetry("broker.example:1883", topic_prefix="fleettest",
                          _client_factory=self.factory)
        self.assertEqual(len(self.published), 1)
        self.assertEqual(self.published[0]["topic"], "fleettest/stress/esp32-wroom32")

    def test_payload_shape(self):
        rs = ResultSet("soak")
        rs.add(Result("r", _dev(board="bitaxe-601"), STATUS_FAIL, "bad",
                      {"heap_free_min": 10000, "reboot_count": 2}))
        rs.push_telemetry("broker.example:1883", _client_factory=self.factory)
        self.assertEqual(len(self.published), 1)
        payload = json.loads(self.published[0]["payload"].decode())
        self.assertEqual(payload["suite"], "soak")
        self.assertEqual(payload["board"], "bitaxe-601")
        self.assertEqual(payload["host"], "192.0.2.1")
        self.assertEqual(payload["status"], "fail")
        self.assertIn("ts", payload)
        self.assertEqual(payload["metrics"]["heap_free_min"], 10000)
        self.assertEqual(payload["metrics"]["reboot_count"], 2)

    def test_results_without_metrics_are_skipped(self):
        rs = ResultSet("functional")
        rs.add(Result("r1", _dev(), STATUS_PASS, ""))           # no metrics
        rs.add(Result("r2", _dev(), STATUS_PASS, "", {}))        # empty metrics
        rs.add(Result("r3", _dev(), STATUS_PASS, "", {"x": 1}))  # has metrics
        rs.push_telemetry("broker.example:1883", _client_factory=self.factory)
        self.assertEqual(len(self.published), 1)

    def test_custom_topic_prefix(self):
        rs = ResultSet("soak")
        rs.add(Result("r", _dev(board="tdongle-s3"), STATUS_PASS, "", {"heap_free_min": 50000}))
        rs.push_telemetry("broker.example:1883", topic_prefix="myprefix",
                          _client_factory=self.factory)
        self.assertEqual(self.published[0]["topic"], "myprefix/soak/tdongle-s3")


class TestPushTelemetryNoPublish(unittest.TestCase):
    """--no-publish-metrics path: no broker call made."""

    def test_empty_broker_url_skips(self):
        factory, published = _make_published_factory()
        rs = ResultSet("soak")
        rs.add(Result("r", _dev(), STATUS_PASS, "", {"heap_free_min": 60000}))
        rs.push_telemetry("", _client_factory=factory)
        self.assertEqual(published, [])

    def test_none_broker_url_skips(self):
        factory, published = _make_published_factory()
        rs = ResultSet("soak")
        rs.add(Result("r", _dev(), STATUS_PASS, "", {"heap_free_min": 60000}))
        rs.push_telemetry(None, _client_factory=factory)
        self.assertEqual(published, [])


class TestPushTelemetryNoBrokerConfigured(unittest.TestCase):
    """When no broker URL is provided, push_telemetry is a no-op; run succeeds."""

    def test_no_publish_no_exception(self):
        rs = ResultSet("soak")
        rs.add(Result("r", _dev(), STATUS_PASS, "", {"heap_free_min": 60000}))
        # No broker: should not raise, no publish
        try:
            rs.push_telemetry(None)
        except Exception as e:
            self.fail(f"push_telemetry raised unexpectedly: {e}")


class TestPushTelemetryExceptionHandling(unittest.TestCase):
    """Publish exception must not propagate — run still succeeds."""

    def test_publish_exception_is_warned_not_raised(self):
        class BombFactory:
            def __init__(self):
                self.on_connect = None
                self.on_publish = None

            def connect(self, *a, **kw):
                raise OSError("network unreachable")

            def loop_start(self): pass
            def loop_stop(self): pass
            def disconnect(self): pass

        rs = ResultSet("soak")
        rs.add(Result("r", _dev(), STATUS_PASS, "", {"heap_free_min": 60000}))
        # connect failure → connect_and_publish returns (False, "broker connect failed: …")
        # → results.py logs a WARNING via bbdevice.device.results logger
        with self.assertLogs("bbdevice.device.results", level="WARNING") as cm:
            try:
                rs.push_telemetry("broker.example:1883", _client_factory=BombFactory)
            except Exception as e:
                self.fail(f"exception must not propagate: {e}")
        self.assertTrue(any("connect" in m.lower() or "publish" in m.lower()
                            for m in cm.output))

    def test_paho_missing_logs_warning(self):
        """When paho-mqtt is not importable, push_telemetry warns and does not raise."""
        rs = ResultSet("soak")
        rs.add(Result("r", _dev(), STATUS_PASS, "", {"heap_free_min": 60000}))
        import bbdevice.device.mqtt as mqtt_mod
        orig = mqtt_mod._import_paho
        try:
            mqtt_mod._import_paho = lambda: (None, "paho-mqtt not installed (test stub)")
            with self.assertLogs("bbdevice.device.results", level="WARNING"):
                rs.push_telemetry("broker.example:1883")
        finally:
            mqtt_mod._import_paho = orig


class TestPushTelemetryRename(unittest.TestCase):
    """Verify push_influxdb is gone and push_telemetry exists."""

    def test_push_telemetry_exists(self):
        rs = ResultSet("t")
        self.assertTrue(hasattr(rs, "push_telemetry"))
        self.assertTrue(callable(rs.push_telemetry))

    def test_push_influxdb_removed(self):
        rs = ResultSet("t")
        self.assertFalse(hasattr(rs, "push_influxdb"),
                         "push_influxdb must be removed — use push_telemetry")


if __name__ == "__main__":
    unittest.main()
