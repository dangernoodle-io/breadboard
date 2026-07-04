"""Tests for bbdevice.device.profiles — generic container + default resolver.

Ported (generic subset) from TaipanMiner scripts/fleet/tests/test_profiles.py —
board-taxonomy detection (bitaxe/ASIC/no-PSRAM matching) is TM-only and stays
in fleet's profile_for(); this file covers Profile/Profiles container
behaviour and the no-op default_profile() resolver.
"""
from __future__ import annotations
import os
import tempfile
import unittest

from bbdevice.device.profiles import Profile, Profiles, default_profile


class TestProfileDataclassDefaults(unittest.TestCase):
    def test_defaults(self):
        p = Profile(board="test-board")
        self.assertEqual(p.board, "test-board")
        self.assertFalse(p.has_psram)
        self.assertEqual(p.max_concurrent, 4)
        self.assertEqual(p.max_rps, 10.0)
        self.assertFalse(p.single_worker)
        self.assertIsNone(p.poll_interval)
        self.assertIsNone(p.heap_floor)
        self.assertIsNone(p.readiness_heap_floor)


class TestDefaultProfile(unittest.TestCase):
    """default_profile() is a no-op/identity resolver — no board taxonomy."""

    def test_unknown_board_returns_bare_profile(self):
        p = default_profile("some-unknown-board-xyz")
        self.assertEqual(p.board, "some-unknown-board-xyz")
        self.assertFalse(p.has_psram)
        self.assertIsNone(p.heap_floor)

    def test_bitaxe_board_gets_no_special_treatment(self):
        """Unlike TM's profile_for(), default_profile() does not detect ASIC boards."""
        p = default_profile("bitaxe-403")
        self.assertEqual(p.board, "bitaxe-403")
        self.assertFalse(p.has_psram)
        self.assertEqual(p.max_concurrent, 4)  # generic default, no ASIC override

    def test_profiles_override_takes_precedence(self):
        profiles = Profiles(overrides={"esp32-c3": {"heap_floor": 30_000, "single_worker": True}})
        p = default_profile("esp32-c3-supermini", profiles=profiles)
        self.assertEqual(p.heap_floor, 30_000)
        self.assertTrue(p.single_worker)

    def test_profiles_no_match_falls_back_to_bare(self):
        profiles = Profiles(overrides={"esp32-c3": {"heap_floor": 30_000}})
        p = default_profile("tdongle-s3", profiles=profiles)
        self.assertIsNone(p.heap_floor)


class TestProfilesContainer(unittest.TestCase):
    def test_empty_profiles_get_returns_none(self):
        profiles = Profiles()
        self.assertIsNone(profiles.get("anything"))

    def test_get_builds_profile_from_overrides(self):
        profiles = Profiles(overrides={"esp32-s2": {"heap_floor": 8000, "readiness_heap_floor": 8000}})
        p = profiles.get("esp32-s2")
        self.assertIsNotNone(p)
        self.assertEqual(p.board, "esp32-s2")
        self.assertEqual(p.heap_floor, 8000)
        self.assertEqual(p.readiness_heap_floor, 8000)

    def test_get_ignores_unknown_keys(self):
        profiles = Profiles(overrides={"weird": {"heap_floor": 1000, "vcore_floor_mv": 500}})
        p = profiles.get("weird")
        self.assertEqual(p.heap_floor, 1000)
        self.assertFalse(hasattr(p, "vcore_floor_mv"))

    def test_get_missing_key_returns_none(self):
        profiles = Profiles(overrides={"a": {}})
        self.assertIsNone(profiles.get("b"))

    def test_load_missing_file_returns_empty(self):
        profiles = Profiles.load("/nonexistent/profiles.yaml")
        self.assertEqual(profiles.overrides, {})

    def test_load_yaml_file(self):
        try:
            import yaml  # noqa: F401
        except ImportError:
            self.skipTest("pyyaml not installed")
        content = "esp32-s2:\n  heap_floor: 8000\n"
        with tempfile.NamedTemporaryFile(
            suffix=".yaml", delete=False, mode="w"
        ) as f:
            f.write(content)
            path = f.name
        try:
            profiles = Profiles.load(path)
            p = profiles.get("esp32-s2")
            self.assertEqual(p.heap_floor, 8000)
        finally:
            os.unlink(path)

    def test_repr(self):
        profiles = Profiles(overrides={"a": {}, "b": {}})
        self.assertIn("a", repr(profiles))
        self.assertIn("b", repr(profiles))


if __name__ == "__main__":
    unittest.main()
