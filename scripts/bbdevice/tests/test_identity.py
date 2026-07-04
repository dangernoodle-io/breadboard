"""Tests for bbdevice.device.identity._read_identity (HTTP /api/info read).

Minimal test written for the 4a extraction — _read_identity previously had
no dedicated test file in fleet (it was only exercised indirectly via
fleetlib.safety's tests, patched out at the call site).
"""
from __future__ import annotations
import unittest
from unittest.mock import MagicMock, patch

from bbdevice.device.discovery import Device
from bbdevice.device.identity import _read_identity


def _dev() -> Device:
    return Device(hostname="test-host", ip="192.0.2.1", port=80,
                  board="test-board", version="v1.0.0")


class TestReadIdentity(unittest.TestCase):
    def test_reachable_returns_board_and_hostname(self):
        info = {"build": {"board": "esp32-wroom32"}, "hostname": "taipan-81"}
        with patch("bbdevice.device.identity.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            board, hostname = _read_identity(_dev())
        self.assertEqual(board, "esp32-wroom32")
        self.assertEqual(hostname, "taipan-81")

    def test_unreachable_returns_none_none(self):
        with patch("bbdevice.device.identity.Client") as MockClient:
            MockClient.return_value.get_json.return_value = None
            board, hostname = _read_identity(_dev())
        self.assertIsNone(board)
        self.assertIsNone(hostname)

    def test_no_build_object_board_none(self):
        info = {"hostname": "taipan-81"}
        with patch("bbdevice.device.identity.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            board, hostname = _read_identity(_dev())
        self.assertIsNone(board)
        self.assertEqual(hostname, "taipan-81")

    def test_hostname_falls_back_to_host_key(self):
        info = {"build": {"board": "esp32-wroom32"}, "host": "legacy-host"}
        with patch("bbdevice.device.identity.Client") as MockClient:
            MockClient.return_value.get_json.return_value = info
            board, hostname = _read_identity(_dev())
        self.assertEqual(hostname, "legacy-host")


if __name__ == "__main__":
    unittest.main()
