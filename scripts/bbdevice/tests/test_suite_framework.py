"""Tests for bbdevice.suite_framework — SettleConfig.wait_ready restoration.

SettleConfig.wait_ready was deferred (dropped) when suite_framework.py was
first relocated from fleet's suites/__init__.py because it needed
readiness_core/criteria_core, which hadn't landed yet. These tests cover
the restored method now that both modules exist.
"""
from __future__ import annotations
import unittest
from unittest.mock import MagicMock

from bbdevice.suite_framework import SettleConfig, SuiteContext, gate_enabled
from bbdevice.device.criteria_core import Criteria
from bbdevice.device.readiness_core import Readiness


def _ready_client():
    c = MagicMock()

    def _get_json(path, timeout=5):
        mapping = {
            "/api/info": {"uptime_ms": 60_000},
            "/api/diag/heap": {"internal": {"free": 80_000}},
        }
        return mapping.get(path)

    c.get_json = _get_json
    return c


class TestSettleConfigWaitReady(unittest.TestCase):
    def test_disabled_returns_ready_immediately(self):
        settle = SettleConfig(settle_delay=120, enabled=False)
        r = settle.wait_ready(_ready_client())
        self.assertIsInstance(r, Readiness)
        self.assertTrue(r.ready)
        self.assertEqual(r.elapsed_s, 0.0)

    def test_zero_settle_delay_returns_ready_immediately(self):
        settle = SettleConfig(settle_delay=0, enabled=True)
        r = settle.wait_ready(_ready_client())
        self.assertTrue(r.ready)

    def test_enabled_wires_to_readiness_core(self):
        settle = SettleConfig(settle_delay=1, enabled=True)
        r = settle.wait_ready(_ready_client(), timeout=10)
        self.assertTrue(r.ready)
        self.assertGreaterEqual(r.elapsed_s, 1)

    def test_default_criteria_used_when_none_supplied(self):
        settle = SettleConfig(settle_delay=1, enabled=True)
        r = settle.wait_ready(_ready_client(), criteria=None, timeout=10)
        self.assertTrue(r.ready)

    def test_extra_checks_threaded_through(self):
        settle = SettleConfig(settle_delay=0, enabled=True)
        settle.settle_delay = 1  # force enabled path with a real settle window
        failing_check = lambda s, c: "extension gate failed"
        r = settle.wait_ready(
            _ready_client(), criteria=Criteria(readiness_heap_floor=50_000),
            extra_checks=[failing_check], timeout=3,
        )
        self.assertFalse(r.ready)
        self.assertIn("extension gate failed", r.reason)

    def test_criteria_settle_delay_overridden_by_settle_config(self):
        """SettleConfig.settle_delay wins over criteria.settle_delay when they differ."""
        settle = SettleConfig(settle_delay=1, enabled=True)
        crit = Criteria(settle_delay=999, readiness_heap_floor=50_000)
        r = settle.wait_ready(_ready_client(), criteria=crit, timeout=10)
        self.assertTrue(r.ready)
        self.assertGreaterEqual(r.elapsed_s, 1)
        self.assertLess(r.elapsed_s, 30)


class TestSuiteContextTypedFields(unittest.TestCase):
    """criteria/profiles are real types (post-restore), not bare `object`."""

    def test_suite_context_accepts_typed_criteria_and_profiles(self):
        from bbdevice.device.profiles import Profiles

        ctx = SuiteContext(
            devices=[],
            criteria=Criteria(),
            guard=MagicMock(),
            results=MagicMock(),
            fields=None,
            gates=set(),
            settle=SettleConfig(),
            out_json=None,
            out_junit=None,
            baseline=None,
            profiles=Profiles(),
        )
        self.assertIsInstance(ctx.criteria, Criteria)
        self.assertIsInstance(ctx.profiles, Profiles)

    def test_gate_enabled_unchanged(self):
        ctx = SuiteContext(
            devices=[], criteria=Criteria(), guard=MagicMock(), results=MagicMock(),
            fields=None, gates={"heap_floor"}, settle=SettleConfig(),
            out_json=None, out_junit=None, baseline=None,
        )
        self.assertTrue(gate_enabled(ctx, "heap_floor"))
        self.assertFalse(gate_enabled(ctx, "other"))


if __name__ == "__main__":
    unittest.main()
