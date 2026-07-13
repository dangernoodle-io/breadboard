"""Tests for scripts/coverage_gate.py: the zero-total/zero-percent tooling-
failure guard (B1-867) and the line+branch gate (B1-642, PR #845)."""
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import coverage_gate  # noqa: E402


class _CoverageGateTestBase(unittest.TestCase):
    def _run(self, summary, extra_env=None):
        with tempfile.TemporaryDirectory() as tmp:
            summary_path = os.path.join(tmp, "gcovr-summary.json")
            coveralls_path = os.path.join(tmp, "gcovr-coveralls.json")
            with open(summary_path, "w", encoding="utf-8") as fh:
                json.dump(summary, fh)

            argv = [
                "coverage_gate.py",
                "--summary-out", summary_path,
                "--coveralls-out", coveralls_path,
            ]
            env = {"COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13"}
            if extra_env:
                env.update(extra_env)

            out, err = io.StringIO(), io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(
                     coverage_gate.subprocess, "run",
                     return_value=mock.Mock(returncode=0)), \
                 mock.patch.dict(os.environ, env), \
                 redirect_stdout(out), redirect_stderr(err):
                rc = coverage_gate.main()
            return rc, out.getvalue(), err.getvalue()


class TestZeroTotalGuard(_CoverageGateTestBase):
    """B1-867: a real measurement is never literally zero -- zero relevant
    lines/branches or an exact-0% result means the toolchain is wrong, not
    that coverage regressed to zero."""

    def test_zero_total_is_a_tooling_failure_not_a_result(self):
        rc, _out, err = self._run({
            "line_total": 0, "branch_total": 0,
            "line_percent": 0.0, "branch_percent": 0.0,
        })
        self.assertEqual(rc, 1)
        self.assertIn("ABORT", err)
        self.assertIn("ZERO relevant", err)

    def test_zero_percent_over_nonzero_total_is_a_tooling_failure(self):
        rc, _out, err = self._run({
            "line_total": 500, "branch_total": 80,
            "line_percent": 0.0, "branch_percent": 0.0,
        })
        self.assertEqual(rc, 1)
        self.assertIn("ABORT", err)
        self.assertIn("exactly 0%", err)

    def test_nonzero_real_result_does_not_trip_the_guard(self):
        rc, _out, _err = self._run({
            "line_total": 500, "branch_total": 80,
            "line_percent": 100.0, "branch_percent": 100.0,
        })
        self.assertEqual(rc, 0)


class TestLineAndBranchGate(_CoverageGateTestBase):
    """B1-642 / PR #845: gate on BOTH metrics -- a line-only shortfall must
    fail even when branch coverage is perfect."""

    def test_line_only_shortfall_fails(self):
        rc, _out, err = self._run({
            "line_total": 1000, "branch_total": 200,
            "line_percent": 99.97, "branch_percent": 100.0,
        })
        self.assertEqual(rc, 1)
        self.assertIn("line coverage 99.97%", err)

    def test_branch_only_shortfall_fails(self):
        rc, _out, err = self._run({
            "line_total": 1000, "branch_total": 200,
            "line_percent": 100.0, "branch_percent": 98.0,
        })
        self.assertEqual(rc, 1)
        self.assertIn("branch coverage 98.00%", err)

    def test_full_coverage_passes(self):
        rc, _out, _err = self._run({
            "line_total": 1000, "branch_total": 200,
            "line_percent": 100.0, "branch_percent": 100.0,
        })
        self.assertEqual(rc, 0)

    def test_min_thresholds_are_overridable_for_a_deliberate_exception(self):
        rc, _out, _err = self._run(
            {
                "line_total": 1000, "branch_total": 200,
                "line_percent": 95.0, "branch_percent": 95.0,
            },
            extra_env={"COVERAGE_MIN_LINE": "90", "COVERAGE_MIN_BRANCH": "90"},
        )
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
