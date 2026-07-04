"""Tests for bbdevice.device.discovery.

Ported from TaipanMiner scripts/fleet/tests/test_info_field.py (discovery
portion) and test_resolve_devices.py (classification + from_hosts_detailed
portions). Command-layer tests (commands.discover / commands.status,
resolve_devices with --board filtering) are out of scope for this move —
they depend on fleet's CLI commands and profiles.py (PR 4b+), not on
discovery.py itself, and stay behind in fleetlib.
"""
from __future__ import annotations
import json
import socket
import unittest
import urllib.error
from unittest.mock import MagicMock, patch

from bbdevice.device.discovery import (
    Device,
    _classify_enrich_exception,
    _enrich,
    _enrich_with_reason,
    discover,
    from_hosts_detailed,
    verify_identity,
)

SERVICE_TYPE = "_taipanminer._tcp.local."


# ---------------------------------------------------------------------------
# _enrich / _enrich_with_reason — board/version resolved from build.*
# ---------------------------------------------------------------------------

class TestDiscoveryEnrich(unittest.TestCase):
    def _build_info_response(self):
        return {
            "hostname": "taipan-81",
            "build": {"board": "esp32-wroom32", "version": "dev-f218d41-bb-7043c1e"},
            "uptime_ms": 5000,
        }

    def test_enrich_board_version_from_build(self):
        info = self._build_info_response()
        with patch("bbdevice.device.discovery.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            d = _enrich("192.0.2.81")
        self.assertIsNotNone(d)
        self.assertEqual(d.board, "esp32-wroom32")
        self.assertEqual(d.version, "dev-f218d41-bb-7043c1e")

    def test_enrich_with_reason_board_version_from_build(self):
        info = self._build_info_response()
        fake_resp = MagicMock()
        fake_resp.__enter__ = lambda s: s
        fake_resp.__exit__ = MagicMock(return_value=False)
        fake_resp.read.return_value = json.dumps(info).encode()
        with patch("urllib.request.urlopen", return_value=fake_resp):
            d, failure = _enrich_with_reason("192.0.2.81")
        self.assertIsNotNone(d)
        self.assertIsNone(failure)
        self.assertEqual(d.board, "esp32-wroom32")
        self.assertEqual(d.version, "dev-f218d41-bb-7043c1e")

    def test_enrich_missing_build_board_unknown(self):
        """When build is absent, board/version fall back to 'unknown'."""
        info = {"hostname": "old-device", "uptime_ms": 1000}
        with patch("bbdevice.device.discovery.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            d = _enrich("192.0.2.107")
        self.assertIsNotNone(d)
        self.assertEqual(d.board, "unknown")
        self.assertEqual(d.version, "unknown")


# ---------------------------------------------------------------------------
# verify_identity
# ---------------------------------------------------------------------------

class TestVerifyIdentity(unittest.TestCase):
    def test_board_match_via_build(self):
        info = {"build": {"board": "esp32-wroom32"}, "hostname": "taipan-81"}
        d = Device(hostname="taipan-81", ip="192.0.2.81", port=80,
                   board="esp32-wroom32", version="v1.0.0")
        with patch("bbdevice.device.discovery.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            result = verify_identity(d, expect_board="esp32-wroom32")
        self.assertTrue(result)

    def test_board_mismatch_via_build(self):
        info = {"build": {"board": "bitaxe-601"}, "hostname": "taipan-81"}
        d = Device(hostname="taipan-81", ip="192.0.2.81", port=80,
                   board="esp32-wroom32", version="v1.0.0")
        with patch("bbdevice.device.discovery.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            result = verify_identity(d, expect_board="esp32-wroom32")
        self.assertFalse(result)

    def test_no_build_board_mismatch(self):
        """Without build object, board is None which != expect_board."""
        info = {"hostname": "taipan-81"}
        d = Device(hostname="taipan-81", ip="192.0.2.81", port=80,
                   board="esp32-wroom32", version="v1.0.0")
        with patch("bbdevice.device.discovery.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            result = verify_identity(d, expect_board="esp32-wroom32")
        self.assertFalse(result)


# ---------------------------------------------------------------------------
# Reason classification via _classify_enrich_exception
# ---------------------------------------------------------------------------

class TestReasonClassification(unittest.TestCase):
    def test_socket_timeout_is_timeout(self):
        cat, reason = _classify_enrich_exception(socket.timeout("timed out"), 5)
        self.assertEqual(cat, "timeout")
        self.assertIn("5s", reason)

    def test_urllib_urlerror_wrapping_socket_timeout(self):
        exc = urllib.error.URLError(socket.timeout("timed out"))
        cat, reason = _classify_enrich_exception(exc, 8)
        self.assertEqual(cat, "timeout")
        self.assertIn("8s", reason)

    def test_urllib_urlerror_connection_refused(self):
        inner = OSError(111, "Connection refused")
        inner.errno = 111
        exc = urllib.error.URLError(inner)
        cat, reason = _classify_enrich_exception(exc, 5)
        self.assertEqual(cat, "refused")
        self.assertIn("refused", reason)

    def test_urllib_urlerror_no_route(self):
        inner = OSError(113, "No route to host")
        exc = urllib.error.URLError(inner)
        cat, reason = _classify_enrich_exception(exc, 5)
        self.assertEqual(cat, "no_route")

    def test_http_error_is_http_error(self):
        exc = urllib.error.HTTPError(
            url="http://x", code=503, msg="Service Unavailable", hdrs=None, fp=None
        )
        cat, reason = _classify_enrich_exception(exc, 5)
        self.assertEqual(cat, "http_error")
        self.assertIn("503", reason)

    def test_connection_refused_error_is_refused(self):
        exc = ConnectionRefusedError(111, "Connection refused")
        cat, reason = _classify_enrich_exception(exc, 5)
        self.assertEqual(cat, "refused")

    def test_timeout_error_is_timeout(self):
        exc = TimeoutError("timed out")
        cat, reason = _classify_enrich_exception(exc, 10)
        self.assertEqual(cat, "timeout")
        self.assertIn("10s", reason)

    def test_urlerror_timeout_string_in_reason(self):
        inner = OSError("timed out")
        exc = urllib.error.URLError(inner)
        cat, reason = _classify_enrich_exception(exc, 5)
        self.assertEqual(cat, "timeout")


# ---------------------------------------------------------------------------
# from_hosts_detailed integration (mock urllib.request.urlopen)
# ---------------------------------------------------------------------------

class TestFromHostsDetailed(unittest.TestCase):
    def _mock_urlopen_side_effect(self, responses: dict):
        """Build a side_effect that maps URL prefixes to responses or exceptions."""
        def _side_effect(url, timeout=5):
            for prefix, val in responses.items():
                if prefix in url:
                    if isinstance(val, Exception):
                        raise val
                    cm = MagicMock()
                    cm.__enter__ = lambda s: s
                    cm.__exit__ = MagicMock(return_value=False)
                    cm.read.return_value = json.dumps(val).encode()
                    return cm
            raise socket.timeout("timed out")

        return _side_effect

    def test_all_succeed(self):
        info = {"hostname": "miner-a", "board": "esp32-wroom32", "version": "v0.1.0"}
        se = self._mock_urlopen_side_effect({"192.0.2.10": info, "192.0.2.11": info})
        with patch("urllib.request.urlopen", side_effect=se):
            r = from_hosts_detailed(["192.0.2.10", "192.0.2.11"])
        self.assertEqual(len(r.devices), 2)
        self.assertEqual(len(r.failures), 0)
        self.assertFalse(r.from_mdns)

    def test_all_fail_timeout(self):
        se = self._mock_urlopen_side_effect({
            "192.0.2.250": socket.timeout("timed out"),
            "192.0.2.251": socket.timeout("timed out"),
        })
        with patch("urllib.request.urlopen", side_effect=se):
            r = from_hosts_detailed(["192.0.2.250", "192.0.2.251"])
        self.assertEqual(len(r.devices), 0)
        self.assertEqual(len(r.failures), 2)
        for f in r.failures:
            self.assertEqual(f.category, "timeout")
            self.assertIn("timeout", f.reason)

    def test_partial_one_succeeds_one_fails(self):
        info = {"hostname": "miner-a", "board": "esp32-wroom32", "version": "v0.1.0"}
        se = self._mock_urlopen_side_effect({
            "192.0.2.10": info,
            "192.0.2.250": socket.timeout("timed out"),
        })
        with patch("urllib.request.urlopen", side_effect=se):
            r = from_hosts_detailed(["192.0.2.10", "192.0.2.250"])
        self.assertEqual(len(r.devices), 1)
        self.assertEqual(r.devices[0].ip, "192.0.2.10")
        self.assertEqual(len(r.failures), 1)
        self.assertEqual(r.failures[0].host, "192.0.2.250")
        self.assertEqual(r.failures[0].category, "timeout")

    def test_refused_failure_category(self):
        inner = OSError(111, "Connection refused")
        inner.errno = 111
        exc = urllib.error.URLError(inner)
        se = self._mock_urlopen_side_effect({"192.0.2.250": exc})
        with patch("urllib.request.urlopen", side_effect=se):
            r = from_hosts_detailed(["192.0.2.250"])
        self.assertEqual(r.failures[0].category, "refused")

    def test_resolve_result_not_from_mdns(self):
        se = self._mock_urlopen_side_effect({"192.0.2.250": socket.timeout("x")})
        with patch("urllib.request.urlopen", side_effect=se):
            r = from_hosts_detailed(["192.0.2.250"])
        self.assertFalse(r.from_mdns)


# ---------------------------------------------------------------------------
# discover() — service_type is a required param; empty result is graceful
# ---------------------------------------------------------------------------

class TestDiscoverServiceType(unittest.TestCase):
    def _no_op_browser(self, monkey_zc, monkey_browser, service_type_capture):
        def _fake_service_browser(zc, stype, listener):
            service_type_capture.append(stype)
            return MagicMock()
        return _fake_service_browser

    def test_discover_requires_service_type_positional(self):
        with self.assertRaises(TypeError):
            discover()  # no service_type -> TypeError, required param

    def test_discover_empty_returns_empty_list_no_exception(self):
        captured = []
        with patch("zeroconf.Zeroconf") as MockZC, \
             patch("zeroconf.ServiceBrowser", side_effect=self._no_op_browser(None, None, captured)), \
             patch("time.sleep"):
            MockZC.return_value = MagicMock()
            result = discover(SERVICE_TYPE, timeout=0)
        self.assertEqual(result, [])
        self.assertEqual(captured, [SERVICE_TYPE])

    def test_discover_passes_caller_service_type_not_hardcoded(self):
        captured = []
        custom = "_custom._tcp.local."
        with patch("zeroconf.Zeroconf") as MockZC, \
             patch("zeroconf.ServiceBrowser", side_effect=self._no_op_browser(None, None, captured)), \
             patch("time.sleep"):
            MockZC.return_value = MagicMock()
            discover(custom, timeout=0)
        self.assertEqual(captured, [custom])


if __name__ == "__main__":
    unittest.main()
