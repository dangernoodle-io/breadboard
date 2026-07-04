"""Offline unit tests for B1-360 info_field accessor (TA-467).

Ported from TaipanMiner scripts/fleet/tests/test_info_field.py — only the
info_field portion (client.py); discovery/ota/commands coverage in the
original file is out of scope for this move and stays behind in fleetlib.
"""
from __future__ import annotations
import unittest

from bbdevice.device.client import info_field


class TestInfoField(unittest.TestCase):
    def _build_info(self, **build_kwargs):
        return {"build": build_kwargs, "uptime_ms": 1000, "mac": "aa:bb:cc"}

    def test_build_shape_returns_build_value(self):
        info = self._build_info(version="v1.2.3", board="esp32-wroom32")
        self.assertEqual(info_field(info, "version"), "v1.2.3")
        self.assertEqual(info_field(info, "board"), "esp32-wroom32")

    def test_build_shape_app_sha256(self):
        info = self._build_info(app_sha256="dd159641e")
        self.assertEqual(info_field(info, "app_sha256"), "dd159641e")

    def test_missing_build_returns_default(self):
        info = {"uptime_ms": 1000, "version": "old", "board": "old-board"}
        self.assertIsNone(info_field(info, "version"))
        self.assertIsNone(info_field(info, "board"))

    def test_missing_build_custom_default(self):
        info = {"uptime_ms": 1000}
        self.assertEqual(info_field(info, "version", "unknown"), "unknown")

    def test_missing_key_in_build_returns_default(self):
        info = self._build_info(version="v1.0.0")
        self.assertIsNone(info_field(info, "board"))
        self.assertEqual(info_field(info, "board", "unknown"), "unknown")

    def test_build_not_a_dict_returns_default(self):
        info = {"build": "not-a-dict", "version": "top"}
        self.assertIsNone(info_field(info, "version"))

    def test_build_none_returns_default(self):
        info = {"build": None}
        self.assertIsNone(info_field(info, "version"))

    def test_empty_info_returns_default(self):
        self.assertIsNone(info_field({}, "version"))

    def test_dynamic_top_level_fields_not_routed(self):
        """uptime_ms / mac stay top-level — info_field must not see them."""
        info = {"build": {}, "uptime_ms": 99000, "mac": "aa:bb:cc:dd:ee:ff"}
        # Dynamic fields are read directly from info, not through info_field
        self.assertEqual(info.get("uptime_ms"), 99000)
        self.assertEqual(info.get("mac"), "aa:bb:cc:dd:ee:ff")
        # info_field returns nothing for absent build keys
        self.assertIsNone(info_field(info, "uptime_ms"))


if __name__ == "__main__":
    unittest.main()
