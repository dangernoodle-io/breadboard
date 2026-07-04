"""Tests for bbdevice.device.criteria_core — defaults, YAML merge, profile override.

Ported (generic subset) from TaipanMiner scripts/fleet/tests/test_criteria.py —
mining fields (hashrate_floor_pct, vcore_floor_mv, readiness_hashrate_min,
readiness_vcore_floor) are not first-class here; the overlay-dict tests below
cover that extension point instead.
"""
from __future__ import annotations
import os
import tempfile
import unittest

from bbdevice.device.criteria_core import Criteria, load, for_profile
from bbdevice.device.profiles import Profile


class TestCriteriaDefaults(unittest.TestCase):
    def test_poll_interval(self):
        self.assertEqual(Criteria().poll_interval, 60.0)

    def test_duration(self):
        self.assertEqual(Criteria().duration, 3600.0)

    def test_heap_floor(self):
        self.assertEqual(Criteria().heap_floor, 50_000)

    def test_bad_reset_reasons(self):
        rr = Criteria().bad_reset_reasons
        for r in ("panic", "task_wdt", "int_wdt", "brownout"):
            self.assertIn(r, rr)

    def test_max_missed_polls(self):
        self.assertEqual(Criteria().max_missed_polls, 4)

    def test_overlay_defaults_empty(self):
        self.assertEqual(Criteria().overlay, {})


class TestCriteriaLoad(unittest.TestCase):
    def test_missing_file_returns_defaults(self):
        c = load("/nonexistent/criteria.yaml")
        self.assertEqual(c.poll_interval, 60.0)
        self.assertEqual(c.heap_floor, 50_000)

    def test_yaml_merge(self):
        try:
            import yaml  # noqa: F401
        except ImportError:
            self.skipTest("pyyaml not installed")
        content = "poll_interval: 30\nheap_floor: 40000\n"
        with tempfile.NamedTemporaryFile(
            suffix=".yaml", delete=False, mode="w"
        ) as f:
            f.write(content)
            path = f.name
        try:
            c = load(path)
            self.assertEqual(c.poll_interval, 30)
            self.assertEqual(c.heap_floor, 40_000)
            self.assertEqual(c.duration, 3600.0)
        finally:
            os.unlink(path)

    def test_bad_reset_reasons_yaml(self):
        try:
            import yaml  # noqa: F401
        except ImportError:
            self.skipTest("pyyaml not installed")
        content = "bad_reset_reasons: [panic, brownout]\n"
        with tempfile.NamedTemporaryFile(
            suffix=".yaml", delete=False, mode="w"
        ) as f:
            f.write(content)
            path = f.name
        try:
            c = load(path)
            self.assertEqual(c.bad_reset_reasons, {"panic", "brownout"})
        finally:
            os.unlink(path)


class TestCriteriaOverlayExtensionPoint(unittest.TestCase):
    """overlay dict round-trips unknown YAML keys (TM's mining fields land here)."""

    def test_unknown_yaml_keys_land_in_overlay(self):
        try:
            import yaml  # noqa: F401
        except ImportError:
            self.skipTest("pyyaml not installed")
        content = "vcore_floor_mv: 480\nhashrate_floor_pct: 75.0\npoll_interval: 45\n"
        with tempfile.NamedTemporaryFile(
            suffix=".yaml", delete=False, mode="w"
        ) as f:
            f.write(content)
            path = f.name
        try:
            c = load(path)
            self.assertEqual(c.poll_interval, 45)
            self.assertEqual(c.overlay.get("vcore_floor_mv"), 480)
            self.assertEqual(c.overlay.get("hashrate_floor_pct"), 75.0)
        finally:
            os.unlink(path)

    def test_overlay_manual_roundtrip(self):
        c = Criteria()
        c.overlay["vcore_floor_mv"] = 500
        self.assertEqual(c.overlay.get("vcore_floor_mv", 0), 500)
        self.assertEqual(c.overlay.get("missing_key", "default"), "default")

    def test_for_profile_copies_overlay(self):
        c = Criteria()
        c.overlay["vcore_floor_mv"] = 500
        p = Profile(board="test-board")
        c2 = for_profile(c, p)
        c2.overlay["vcore_floor_mv"] = 600
        self.assertEqual(c.overlay["vcore_floor_mv"], 500, "original overlay must not mutate")


class TestCriteriaForProfile(unittest.TestCase):
    def test_poll_interval_override(self):
        c = Criteria()
        p = Profile(board="test-board", poll_interval=30.0)
        c2 = for_profile(c, p)
        self.assertEqual(c2.poll_interval, 30.0)

    def test_c3_like_overrides_heap_floor(self):
        c = Criteria()
        p = Profile(board="esp32-c3-supermini", heap_floor=30_000)
        c2 = for_profile(c, p)
        self.assertEqual(c2.heap_floor, 30_000)

    def test_no_override_leaves_default(self):
        c = Criteria()
        p = Profile(board="esp32-wroom32")
        c2 = for_profile(c, p)
        self.assertEqual(c2.heap_floor, 50_000)  # default preserved

    def test_for_profile_does_not_mutate_original(self):
        c = Criteria()
        p = Profile(board="esp32-c3-supermini", heap_floor=30_000)
        _ = for_profile(c, p)
        self.assertEqual(c.heap_floor, 50_000)  # original untouched

    def test_bad_reset_reasons_deep_copied(self):
        c = Criteria()
        p = Profile(board="esp32-wroom32")
        c2 = for_profile(c, p)
        c2.bad_reset_reasons.add("custom")
        self.assertNotIn("custom", c.bad_reset_reasons)

    def test_readiness_heap_floor_applied_from_profile(self):
        p = Profile(board="test-board", readiness_heap_floor=30_000)
        c = Criteria()
        c2 = for_profile(c, p)
        self.assertEqual(c2.readiness_heap_floor, 30_000)
        self.assertEqual(c.readiness_heap_floor, 50_000)  # original untouched

    def test_none_fields_leave_criteria_default(self):
        p = Profile(board="test-board")
        c = Criteria()
        c2 = for_profile(c, p)
        self.assertEqual(c2.readiness_heap_floor, 50_000)
        self.assertEqual(c2.poll_interval, 60.0)
        self.assertEqual(c2.heap_floor, 50_000)


if __name__ == "__main__":
    unittest.main()
