"""Offline unit tests for bbdevice.device.ota — mock-client based, no live OTA.

Ported (generic subset) from TaipanMiner scripts/fleet/tests/test_ota.py and
test_ota_push_readiness.py. The mining-specific health assessment (hashrate,
mining reset-reason) stays in fleet's tests — this file instead covers the
health_fn injection point (default vs custom) and the carried TA-532
ScopeViolation guard on push()/pull().

Covers every public function and both apply modes:
  - push: success / reject / no-boot / dry-run / identity-mismatch / no-confirm
  - pull: boot-mode (200) / pull-mode (202) progress-to-terminal / busy (409) /
          no-update / pull-mode download failure / dry-run / guard enforcement
  - mark_valid / recover / reboot: success / dry-run / no-confirm
  - status: read-only merge
  - verify: settle-then-assert pass / version-mismatch fail
  - wait_for_boot: success / version-target / timeout / connection-refused tolerance
  - health_fn: default (reachable-only) vs injected custom callable
  - ScopeViolation: push()/pull() refuse a device outside allowed_hosts before
    any HTTP mutation
"""
from __future__ import annotations
import os
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from bbdevice.device import ota
from bbdevice.device.ota import VerifyResult
from bbdevice.device.criteria_core import Criteria
from bbdevice.device.readiness_core import Readiness
from bbdevice.device.safety import (
    Guard,
    IdentityMismatch,
    RefusedWithoutConfirmation,
    ScopeViolation,
)


# ---------------------------------------------------------------------------
# Mock client
# ---------------------------------------------------------------------------

class MockClient:
    """Mock of bbdevice.device.client.Client.

    gets: path -> value, or list used as a queue (last element repeats).
    reqs: (METHOD, path) -> (status, bytes), or a queue list.
    Every request is recorded in request_log for mutation-skip assertions.
    """

    def __init__(self, ip="192.0.2.1", port=80, gets=None, reqs=None):
        self.ip = ip
        self.port = port
        self.gets = gets or {}
        self.reqs = reqs or {}
        self.request_log = []

    @staticmethod
    def _pop(v):
        if isinstance(v, list):
            if len(v) > 1:
                return v.pop(0)
            return v[0] if v else None
        return v

    def get_json(self, path, timeout=5):
        return self._pop(self.gets.get(path))

    def request(self, method, path, body=None, timeout=10):
        self.request_log.append((method.upper(), path))
        v = self.reqs.get((method.upper(), path), (200, b""))
        out = self._pop(v)
        return out if out is not None else (200, b"")


def _ok_info(version="v0.70.0", reset="power_on"):
    # B1-360 shape: version under build.*, dynamic fields top-level
    return {"build": {"version": version}, "reset_reason": reset, "uptime_ms": 30000}


def _healthy_gets(version="v0.70.0"):
    """GETs dict for a generically-healthy device (no workload-specific fields)."""
    return {
        "/api/health": {"ok": True},
        "/api/info": _ok_info(version),
        "/api/diag/heap": {"internal": {"free": 80000}},
    }


def _live_guard():
    return Guard(dry_run=False, confirm=True, expect_board="esp32-wroom32")


def _patch_identity(ok=True):
    val = ("esp32-wroom32", "test-host") if ok else (None, None)
    return patch("bbdevice.device.identity._read_identity", return_value=val)


# Kill real sleeps in every test (poll loops would otherwise stall).
def setUpModule():
    global _sleep_patcher
    _sleep_patcher = patch("bbdevice.device.ota.time.sleep", lambda *a, **k: None)
    _sleep_patcher.start()


def tearDownModule():
    _sleep_patcher.stop()


# ---------------------------------------------------------------------------
# wait_for_boot
# ---------------------------------------------------------------------------

class TestWaitForBoot(unittest.TestCase):
    def test_back_up_any_version(self):
        c = MockClient(gets={"/api/health": {"ok": True}, "/api/info": _ok_info("v0.69.0")})
        self.assertEqual(ota.wait_for_boot(c), "v0.69.0")

    def test_waits_for_target_version(self):
        c = MockClient(gets={
            "/api/health": {"ok": True},
            "/api/info": [_ok_info("v0.69.0"), _ok_info("v0.70.0")],
        })
        self.assertEqual(ota.wait_for_boot(c, target_version="v0.70.0"), "v0.70.0")

    def test_timeout_returns_none(self):
        c = MockClient(gets={"/api/health": None, "/api/info": None})
        self.assertIsNone(ota.wait_for_boot(c, timeout=0.05))

    def test_tolerates_connection_refused(self):
        def boom(path, timeout=5):
            raise OSError("connection refused")
        c = MockClient()
        c.get_json = boom
        self.assertIsNone(ota.wait_for_boot(c, timeout=0.05))


# ---------------------------------------------------------------------------
# push
# ---------------------------------------------------------------------------

class TestPush(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        self.tmp.write(b"\x00firmware\xff")
        self.tmp.close()

    def tearDown(self):
        os.unlink(self.tmp.name)

    def test_push_success(self):
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0")
        self.assertTrue(r.ok, r.detail)
        self.assertTrue(r.healthy)
        self.assertEqual(r.version, "v0.70.0")
        self.assertIn(("POST", "/api/update/push"), c.request_log)

    def test_push_connection_reset_is_ok(self):
        # status None = device rebooted mid-response (expected boot-mode)
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (None, b"")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0")
        self.assertTrue(r.ok, r.detail)

    def test_push_rejected(self):
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (500, b"err")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name)
        self.assertFalse(r.ok)
        self.assertIn("500", r.detail)

    def test_push_no_boot(self):
        c = MockClient(reqs={("POST", "/api/update/push"): (200, b"")})
        with _patch_identity(True):
            with patch.object(ota, "wait_for_boot", return_value=None):
                r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0")
        self.assertFalse(r.ok)
        self.assertIn("come back up", r.detail)

    def test_push_dry_run_skips_mutation(self):
        c = MockClient(reqs={("POST", "/api/update/push"): (200, b"")})
        g = Guard(dry_run=True, confirm=True, expect_board="esp32-wroom32")
        with _patch_identity(True):
            r = ota.push(c, g, self.tmp.name, target_version="v0.70.0")
        self.assertTrue(r.dry_run)
        self.assertTrue(r.ok)
        self.assertEqual(c.request_log, [])  # NO HTTP mutation

    def test_push_identity_mismatch_refuses(self):
        c = MockClient()
        with patch("bbdevice.device.identity._read_identity",
                   return_value=("wrong-board", "some-host")):
            with self.assertRaises(IdentityMismatch):
                ota.push(c, _live_guard(), self.tmp.name)
        self.assertEqual(c.request_log, [])

    def test_push_no_confirm_refuses(self):
        c = MockClient()
        g = Guard(dry_run=False, confirm=False, expect_board="esp32-wroom32")
        with _patch_identity(True):
            with self.assertRaises(RefusedWithoutConfirmation):
                ota.push(c, g, self.tmp.name)
        self.assertEqual(c.request_log, [])


# ---------------------------------------------------------------------------
# pull — mode detection
# ---------------------------------------------------------------------------

class TestPullBootMode(unittest.TestCase):
    def test_apply_200_boot_mode(self):
        c = MockClient(
            gets=_healthy_gets(),
            reqs={
                ("POST", "/api/update/check"): (200, b'{"available": true, "latest": "v0.70.0"}'),
                ("POST", "/api/update/apply"): (200, b""),
            },
        )
        with _patch_identity(True):
            r = ota.pull(c, _live_guard())
        self.assertTrue(r.ok, r.detail)
        self.assertEqual(r.version, "v0.70.0")
        self.assertIn(("POST", "/api/update/apply"), c.request_log)


class TestPullPullMode(unittest.TestCase):
    def test_apply_202_polls_progress_to_terminal(self):
        gets = _healthy_gets()
        gets["/api/update/progress"] = [
            {"state": "downloading", "percent": 10},
            {"state": "writing", "percent": 80},
            {"state": "done", "percent": 100},
        ]
        c = MockClient(
            gets=gets,
            reqs={
                ("POST", "/api/update/check"): (200, b'{"available": true, "latest": "v0.70.0"}'),
                ("POST", "/api/update/apply"): (202, b""),
            },
        )
        with _patch_identity(True):
            r = ota.pull(c, _live_guard())
        self.assertTrue(r.ok, r.detail)
        self.assertEqual(r.version, "v0.70.0")

    def test_pull_mode_download_failure(self):
        gets = _healthy_gets()
        gets["/api/update/progress"] = [
            {"state": "downloading"},
            {"state": "error", "msg": "bad signature"},
        ]
        c = MockClient(
            gets=gets,
            reqs={
                ("POST", "/api/update/check"): (200, b'{"available": true, "latest": "v0.70.0"}'),
                ("POST", "/api/update/apply"): (202, b""),
            },
        )
        with _patch_identity(True):
            r = ota.pull(c, _live_guard())
        self.assertFalse(r.ok)
        self.assertIn("error", r.detail)


class TestPullBusyAndNoUpdate(unittest.TestCase):
    def test_apply_409_busy(self):
        c = MockClient(
            gets=_healthy_gets(),
            reqs={
                ("POST", "/api/update/check"): (200, b'{"available": true, "latest": "v0.70.0"}'),
                ("POST", "/api/update/apply"): (409, b"busy"),
            },
        )
        with _patch_identity(True):
            r = ota.pull(c, _live_guard())
        self.assertFalse(r.ok)
        self.assertIn("409", r.detail)

    def test_no_update_available_is_benign(self):
        c = MockClient(
            gets={"/api/info": {"build": {"version": "v0.70.0"}, "uptime_ms": 1000}},
            reqs={("POST", "/api/update/check"): (200, b'{"available": false}')},
        )
        with _patch_identity(True):
            r = ota.pull(c, _live_guard())
        self.assertTrue(r.ok)
        self.assertEqual(r.version, "v0.70.0")
        self.assertNotIn(("POST", "/api/update/apply"), c.request_log)


class TestPullGuard(unittest.TestCase):
    def test_dry_run_skips_all_mutation(self):
        c = MockClient(reqs={("POST", "/api/update/check"): (200, b'{"available": true}')})
        g = Guard(dry_run=True, confirm=True, expect_board="esp32-wroom32")
        with _patch_identity(True):
            r = ota.pull(c, g)
        self.assertTrue(r.dry_run)
        self.assertEqual(c.request_log, [])

    def test_identity_mismatch_refuses(self):
        c = MockClient()
        with patch("bbdevice.device.identity._read_identity",
                   return_value=("wrong-board", "some-host")):
            with self.assertRaises(IdentityMismatch):
                ota.pull(c, _live_guard())
        self.assertEqual(c.request_log, [])

    def test_no_confirm_refuses(self):
        c = MockClient()
        g = Guard(dry_run=False, confirm=False, expect_board="esp32-wroom32")
        with _patch_identity(True):
            with self.assertRaises(RefusedWithoutConfirmation):
                ota.pull(c, g)
        self.assertEqual(c.request_log, [])


# ---------------------------------------------------------------------------
# mark_valid / recover / reboot
# ---------------------------------------------------------------------------

class TestMarkValid(unittest.TestCase):
    def test_success(self):
        c = MockClient(reqs={("POST", "/api/update/mark-valid"): (200, b"")})
        with _patch_identity(True):
            self.assertTrue(ota.mark_valid(c, _live_guard()))

    def test_dry_run_no_mutation(self):
        c = MockClient()
        g = Guard(dry_run=True, confirm=True, expect_board="esp32-wroom32")
        with _patch_identity(True):
            self.assertTrue(ota.mark_valid(c, g))
        self.assertEqual(c.request_log, [])

    def test_no_confirm_refuses(self):
        c = MockClient()
        g = Guard(dry_run=False, confirm=False, expect_board="esp32-wroom32")
        with _patch_identity(True):
            with self.assertRaises(RefusedWithoutConfirmation):
                ota.mark_valid(c, g)


class TestRecover(unittest.TestCase):
    def test_success(self):
        c = MockClient(reqs={("POST", "/api/update/recover"): (200, b"")})
        with _patch_identity(True):
            self.assertTrue(ota.recover(c, _live_guard()))

    def test_dry_run_no_mutation(self):
        c = MockClient()
        g = Guard(dry_run=True, confirm=True, expect_board="esp32-wroom32")
        with _patch_identity(True):
            self.assertTrue(ota.recover(c, g))
        self.assertEqual(c.request_log, [])

    def test_failure_status(self):
        c = MockClient(reqs={("POST", "/api/update/recover"): (500, b"err")})
        with _patch_identity(True):
            self.assertFalse(ota.recover(c, _live_guard()))


class TestReboot(unittest.TestCase):
    def test_success_no_settle(self):
        c = MockClient(reqs={("POST", "/api/reboot"): (200, b"")})
        with _patch_identity(True):
            r = ota.reboot(c, _live_guard())
        self.assertTrue(r.ok, r.detail)
        self.assertEqual(r.detail, "reboot issued")

    def test_connection_reset_is_ok(self):
        c = MockClient(reqs={("POST", "/api/reboot"): (None, b"")})
        with _patch_identity(True):
            r = ota.reboot(c, _live_guard())
        self.assertTrue(r.ok)

    def test_rejected(self):
        c = MockClient(reqs={("POST", "/api/reboot"): (500, b"err")})
        with _patch_identity(True):
            r = ota.reboot(c, _live_guard())
        self.assertFalse(r.ok)

    def test_dry_run_no_mutation(self):
        c = MockClient()
        g = Guard(dry_run=True, confirm=True, expect_board="esp32-wroom32")
        with _patch_identity(True):
            r = ota.reboot(c, g)
        self.assertTrue(r.dry_run)
        self.assertEqual(c.request_log, [])

    def test_settle_waits_and_confirms_ready(self):
        c = MockClient(
            gets=_healthy_gets(),
            reqs={("POST", "/api/reboot"): (200, b"")},
        )
        with _patch_identity(True):
            with patch.object(ota, "wait_for_boot", return_value="v0.70.0"):
                with patch("bbdevice.device.ota.wait_until_ready",
                           return_value=Readiness(ready=True, elapsed_s=1.0, reason="ready")):
                    r = ota.reboot(c, _live_guard(), settle=5)
        self.assertTrue(r.ok, r.detail)
        self.assertTrue(r.ready)


# ---------------------------------------------------------------------------
# status (read-only)
# ---------------------------------------------------------------------------

class TestStatus(unittest.TestCase):
    def test_merges_status_and_progress(self):
        c = MockClient(gets={
            "/api/update/status": {"available": True, "latest": "v0.70.0"},
            "/api/update/progress": {"state": "done", "percent": 100},
        })
        merged = ota.status(c)
        self.assertEqual(merged["latest"], "v0.70.0")
        self.assertEqual(merged["progress"]["state"], "done")
        self.assertEqual(c.request_log, [])  # read-only, no mutation

    def test_missing_endpoints_default_empty(self):
        c = MockClient()
        merged = ota.status(c)
        self.assertEqual(merged, {"progress": {}})


# ---------------------------------------------------------------------------
# verify (settle-then-assert, generic — default health_fn only)
# ---------------------------------------------------------------------------

class TestVerify(unittest.TestCase):
    def _criteria(self):
        return Criteria(settle_delay=0, readiness_heap_floor=50_000)

    def test_pass_on_version_match_default_health(self):
        c = MockClient(gets=_healthy_gets("v0.70.0"))
        r = ota.verify(c, self._criteria(), "v0.70.0", settle=0)
        self.assertTrue(r.ok, r.detail)
        self.assertTrue(r.ready)
        self.assertTrue(r.healthy)

    def test_fail_on_version_mismatch(self):
        c = MockClient(gets=_healthy_gets("v0.69.0"))
        r = ota.verify(c, self._criteria(), "v0.70.0", settle=0)
        self.assertFalse(r.ok)
        self.assertIn("version", r.detail)

    def test_settle_override_applied(self):
        # settle=0 must override a large criteria.settle_delay so verify returns fast
        c = MockClient(gets=_healthy_gets("v0.70.0"))
        crit = Criteria(settle_delay=999, readiness_heap_floor=50_000)
        r = ota.verify(c, crit, "v0.70.0", settle=0)
        self.assertTrue(r.ok, r.detail)


# ---------------------------------------------------------------------------
# health_fn injection point — default (reachable-only) vs custom
# ---------------------------------------------------------------------------

class TestHealthFnInjection(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        self.tmp.write(b"\x00firmware\xff")
        self.tmp.close()

    def tearDown(self):
        os.unlink(self.tmp.name)

    def test_verify_default_health_fn_passes_on_reachable_device(self):
        """No health_fn injected -> generic default (reachable only) is used."""
        c = MockClient(gets=_healthy_gets("v0.70.0"))
        r = ota.verify(c, Criteria(settle_delay=0), "v0.70.0", settle=0)
        self.assertTrue(r.ok, r.detail)
        self.assertTrue(r.healthy)

    def test_verify_custom_health_fn_is_called_and_can_fail(self):
        """An injected health_fn(client, info) is actually invoked and its
        result determines VerifyResult.ok / healthy."""
        c = MockClient(gets=_healthy_gets("v0.70.0"))
        calls = []

        def custom_health_fn(client, info):
            calls.append((client, info))
            return False, {"custom_reason": "board-specific check failed"}

        r = ota.verify(c, Criteria(settle_delay=0), "v0.70.0", settle=0,
                       health_fn=custom_health_fn)
        self.assertEqual(len(calls), 1, "health_fn must be called exactly once")
        self.assertIs(calls[0][0], c)
        self.assertIsInstance(calls[0][1], dict)
        self.assertFalse(r.ok)
        self.assertFalse(r.healthy)
        self.assertIn("custom_reason", r.metrics)

    def test_verify_custom_health_fn_passing_yields_ok(self):
        c = MockClient(gets=_healthy_gets("v0.70.0"))

        def custom_health_fn(client, info):
            return True, {"custom_metric": 42}

        r = ota.verify(c, Criteria(settle_delay=0), "v0.70.0", settle=0,
                       health_fn=custom_health_fn)
        self.assertTrue(r.ok, r.detail)
        self.assertEqual(r.metrics.get("custom_metric"), 42)

    def test_push_default_health_fn_used_when_none_injected(self):
        """push() with health_fn=None uses the generic default (reachable-only)."""
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0")
        self.assertTrue(r.ok, r.detail)
        self.assertTrue(r.healthy)

    def test_push_injected_health_fn_is_called_post_boot(self):
        """push() threads a custom health_fn into the post-boot verify path."""
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})
        calls = []

        def custom_health_fn(client, info):
            calls.append(info)
            return True, {"custom": True}

        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                        health_fn=custom_health_fn)
        self.assertTrue(r.ok, r.detail)
        self.assertEqual(len(calls), 1, "injected health_fn must be called from push()")
        self.assertEqual(r.metrics.get("custom"), True)

    def test_push_injected_health_fn_failure_fails_the_push(self):
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})

        def failing_health_fn(client, info):
            return False, {"reason": "board-specific check failed"}

        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                        health_fn=failing_health_fn)
        self.assertFalse(r.ok)
        self.assertFalse(r.healthy)


# ---------------------------------------------------------------------------
# TA-532: ScopeViolation guard carried onto bbdevice push()/pull()
# ---------------------------------------------------------------------------

class TestScopeViolationGuard(unittest.TestCase):
    """A device outside an explicit allowed_hosts scope must hard-fail before
    any HTTP mutation — carried over from fleet's #614 TA-532 fix."""

    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        self.tmp.write(b"\x00firmware\xff")
        self.tmp.close()

    def tearDown(self):
        os.unlink(self.tmp.name)

    def test_push_raises_scope_violation_for_out_of_scope_host(self):
        c = MockClient(ip="192.0.2.99")
        with self.assertRaises(ScopeViolation):
            ota.push(c, _live_guard(), self.tmp.name,
                    allowed_hosts=frozenset({"192.0.2.1", "192.0.2.2"}))
        # Refused before any HTTP call — guard.check() must never have run.
        self.assertEqual(c.request_log, [])

    def test_pull_raises_scope_violation_for_out_of_scope_host(self):
        c = MockClient(ip="192.0.2.99")
        with self.assertRaises(ScopeViolation):
            ota.pull(c, _live_guard(),
                    allowed_hosts=frozenset({"192.0.2.1", "192.0.2.2"}))
        self.assertEqual(c.request_log, [])

    def test_push_allowed_host_within_scope_proceeds(self):
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                        allowed_hosts=frozenset({"192.0.2.1"}))
        self.assertTrue(r.ok, r.detail)

    def test_push_allowed_hosts_none_is_unscoped_and_proceeds(self):
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                        allowed_hosts=None)
        self.assertTrue(r.ok, r.detail)


# ---------------------------------------------------------------------------
# Post-OTA readiness grace always applied (even with settle=None)
# ---------------------------------------------------------------------------

class TestReadinessGraceAlwaysApplied(unittest.TestCase):
    """wait_until_ready must be called with a real timeout regardless of --settle."""

    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        self.tmp.write(b"\x00firmware\xff")
        self.tmp.close()

    def tearDown(self):
        os.unlink(self.tmp.name)

    def test_push_calls_wait_until_ready_with_real_timeout_when_settle_none(self):
        c = MockClient(gets=_healthy_gets(), reqs={("POST", "/api/update/push"): (200, b"")})
        captured = {}

        def fake_wait_until_ready(client, criteria, timeout=300):
            captured["timeout"] = timeout
            return Readiness(ready=True, elapsed_s=15.0, reason="ready")

        with _patch_identity(True):
            with patch.object(ota, "wait_for_boot", return_value="v0.70.0"):
                with patch("bbdevice.device.ota.wait_until_ready",
                           side_effect=fake_wait_until_ready):
                    ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                            settle=None)

        self.assertIn("timeout", captured, "wait_until_ready was not called from push")
        self.assertEqual(captured["timeout"], ota._POST_OTA_READINESS_TIMEOUT)


# ---------------------------------------------------------------------------
# do_mark_valid flag
# ---------------------------------------------------------------------------

class TestMarkValidFlag(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        self.tmp.write(b"\x00firmware\xff")
        self.tmp.close()

    def tearDown(self):
        os.unlink(self.tmp.name)

    def _gets_pending(self, validated=False):
        gets = _healthy_gets()
        gets["/api/health"] = {"ok": True, "validated": validated}
        return gets

    def test_mark_valid_false_does_not_call_mark_valid(self):
        gets = self._gets_pending(validated=False)
        c = MockClient(gets=gets, reqs={("POST", "/api/update/push"): (200, b"")})
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                        do_mark_valid=False)
        self.assertTrue(r.ok, r.detail)
        self.assertTrue(r.pending)
        self.assertNotIn(("POST", "/api/update/mark-valid"), c.request_log)

    def test_mark_valid_true_calls_mark_valid_and_reports_validated(self):
        gets = self._gets_pending(validated=False)
        gets["/api/health"] = [
            {"ok": False, "validated": False},
            {"ok": True, "validated": True},
        ]
        c = MockClient(
            gets=gets,
            reqs={
                ("POST", "/api/update/push"): (200, b""),
                ("POST", "/api/update/mark-valid"): (200, b""),
            },
        )
        with _patch_identity(True):
            r = ota.push(c, _live_guard(), self.tmp.name, target_version="v0.70.0",
                        do_mark_valid=True)
        self.assertTrue(r.ok, r.detail)
        self.assertFalse(r.pending)
        self.assertIn(("POST", "/api/update/mark-valid"), c.request_log)
        self.assertIn("VALIDATED", r.detail)


if __name__ == "__main__":
    unittest.main()
