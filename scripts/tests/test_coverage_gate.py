"""Tests for scripts/coverage_gate.py: the zero-total/zero-percent tooling-
failure guard (B1-867), and the B1-764 coverage-baseline ratchet wiring
(--seed-baseline / --update-baseline / default check)."""
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import coverage_baseline  # noqa: E402
import coverage_gate  # noqa: E402
from fence._base import Marker  # noqa: E402


class _CoverageGateTestBase(unittest.TestCase):
    def _run(self, summary, detail=None, extra_args=None, extra_env=None):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)

        summary_path = os.path.join(tmp, "gcovr-summary.json")
        coveralls_path = os.path.join(tmp, "gcovr-coveralls.json")
        detail_path = os.path.join(tmp, "gcovr-detail.json")
        with open(summary_path, "w", encoding="utf-8") as fh:
            json.dump(summary, fh)
        with open(detail_path, "w", encoding="utf-8") as fh:
            json.dump(detail if detail is not None else {"files": []}, fh)

        argv = [
            "coverage_gate.py",
            "--root", tmp,
            "--summary-out", summary_path,
            "--coveralls-out", coveralls_path,
            "--detail-out", detail_path,
        ]
        if extra_args:
            argv += extra_args
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
        return rc, out.getvalue(), err.getvalue(), tmp


_NONZERO_SUMMARY = {
    "line_total": 500, "branch_total": 80,
    "line_percent": 100.0, "branch_percent": 100.0,
}


class TestZeroTotalGuard(_CoverageGateTestBase):
    """B1-867: a real measurement is never literally zero -- zero relevant
    lines/branches or an exact-0% result means the toolchain is wrong, not
    that coverage regressed to zero."""

    def test_zero_total_is_a_tooling_failure_not_a_result(self):
        rc, _out, err, _tmp = self._run({
            "line_total": 0, "branch_total": 0,
            "line_percent": 0.0, "branch_percent": 0.0,
        })
        self.assertEqual(rc, 1)
        self.assertIn("ABORT", err)
        self.assertIn("ZERO relevant", err)

    def test_zero_percent_over_nonzero_total_is_a_tooling_failure(self):
        rc, _out, err, _tmp = self._run({
            "line_total": 500, "branch_total": 80,
            "line_percent": 0.0, "branch_percent": 0.0,
        })
        self.assertEqual(rc, 1)
        self.assertIn("ABORT", err)
        self.assertIn("exactly 0%", err)

    def test_nonzero_real_result_does_not_trip_the_guard(self):
        rc, _out, _err, _tmp = self._run(_NONZERO_SUMMARY)
        self.assertEqual(rc, 0)


class TestBaselineCheckWiring(_CoverageGateTestBase):
    """The default (no --seed-baseline/--update-baseline) run gates on the
    committed coverage baseline, not a flat percentage floor."""

    def test_no_baseline_and_fully_covered_detail_passes(self):
        rc, out, _err, _tmp = self._run(_NONZERO_SUMMARY, detail={"files": []})
        self.assertEqual(rc, 0)
        self.assertIn("PASS", out)

    def test_new_uncovered_line_with_no_baseline_fails(self):
        detail = {"files": [{"file": "a.c", "lines": [
            {"line_number": 5, "count": 0, "branches": []},
        ]}]}
        rc, _out, err, _tmp = self._run(_NONZERO_SUMMARY, detail=detail)
        self.assertEqual(rc, 1)
        self.assertIn("a.c", err)
        self.assertIn("FAIL", err)

    def test_gap_already_in_committed_baseline_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            coverage_baseline.write_baseline(tmp, {Marker("uncovered_line", "a.c", "5")})
            summary_path = os.path.join(tmp, "gcovr-summary.json")
            coveralls_path = os.path.join(tmp, "gcovr-coveralls.json")
            detail_path = os.path.join(tmp, "gcovr-detail.json")
            with open(summary_path, "w", encoding="utf-8") as fh:
                json.dump(_NONZERO_SUMMARY, fh)
            with open(detail_path, "w", encoding="utf-8") as fh:
                json.dump({"files": [{"file": "a.c", "lines": [
                    {"line_number": 5, "count": 0, "branches": []},
                ]}]}, fh)
            argv = [
                "coverage_gate.py", "--root", tmp,
                "--summary-out", summary_path, "--coveralls-out", coveralls_path,
                "--detail-out", detail_path,
            ]
            out = io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(coverage_gate.subprocess, "run",
                                    return_value=mock.Mock(returncode=0)), \
                 mock.patch.dict(os.environ, {"COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13"}), \
                 redirect_stdout(out):
                rc = coverage_gate.main()
            self.assertEqual(rc, 0)
            self.assertIn("PASS", out.getvalue())

    def test_excluded_line_never_becomes_a_baseline_gap(self):
        detail = {"files": [{"file": "a.c", "lines": [
            {"line_number": 5, "count": 0, "branches": [], "gcovr/excluded": True},
        ]}]}
        rc, out, _err, _tmp = self._run(_NONZERO_SUMMARY, detail=detail)
        self.assertEqual(rc, 0)
        self.assertIn("PASS", out)


class TestSeedAndUpdateBaselineFlags(_CoverageGateTestBase):
    def test_seed_baseline_creates_the_baseline_file(self):
        detail = {"files": [{"file": "a.c", "lines": [
            {"line_number": 5, "count": 0, "branches": []},
        ]}]}
        rc, out, _err, tmp = self._run(_NONZERO_SUMMARY, detail=detail, extra_args=["--seed-baseline"])
        self.assertEqual(rc, 0)
        self.assertIn("seeded", out)
        self.assertEqual(
            coverage_baseline.load_baseline(tmp),
            {Marker("uncovered_line", "a.c", "5")},
        )

    def test_seed_baseline_twice_errors(self):
        detail = {"files": []}
        # First _run uses a fresh tmp root each call, so simulate "already
        # exists" by seeding directly then invoking main() against that root.
        with tempfile.TemporaryDirectory() as tmp:
            coverage_baseline.write_baseline(tmp, set())
            summary_path = os.path.join(tmp, "gcovr-summary.json")
            coveralls_path = os.path.join(tmp, "gcovr-coveralls.json")
            detail_path = os.path.join(tmp, "gcovr-detail.json")
            with open(summary_path, "w", encoding="utf-8") as fh:
                json.dump(_NONZERO_SUMMARY, fh)
            with open(detail_path, "w", encoding="utf-8") as fh:
                json.dump(detail, fh)
            argv = [
                "coverage_gate.py", "--root", tmp,
                "--summary-out", summary_path, "--coveralls-out", coveralls_path,
                "--detail-out", detail_path, "--seed-baseline",
            ]
            err = io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(coverage_gate.subprocess, "run",
                                    return_value=mock.Mock(returncode=0)), \
                 mock.patch.dict(os.environ, {"COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13"}), \
                 redirect_stderr(err):
                rc = coverage_gate.main()
            self.assertEqual(rc, 1)
            self.assertIn("already exists", err.getvalue())

    def test_update_baseline_without_an_existing_baseline_errors(self):
        rc, _out, err, _tmp = self._run(
            _NONZERO_SUMMARY, detail={"files": []}, extra_args=["--update-baseline"],
        )
        self.assertEqual(rc, 1)
        self.assertIn("no baseline yet", err)

    def test_update_baseline_prunes_a_now_covered_gap(self):
        with tempfile.TemporaryDirectory() as tmp:
            coverage_baseline.write_baseline(tmp, {Marker("uncovered_line", "a.c", "5")})
            summary_path = os.path.join(tmp, "gcovr-summary.json")
            coveralls_path = os.path.join(tmp, "gcovr-coveralls.json")
            detail_path = os.path.join(tmp, "gcovr-detail.json")
            with open(summary_path, "w", encoding="utf-8") as fh:
                json.dump(_NONZERO_SUMMARY, fh)
            with open(detail_path, "w", encoding="utf-8") as fh:
                json.dump({"files": []}, fh)  # line 5 now covered
            argv = [
                "coverage_gate.py", "--root", tmp,
                "--summary-out", summary_path, "--coveralls-out", coveralls_path,
                "--detail-out", detail_path, "--update-baseline",
            ]
            out = io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(coverage_gate.subprocess, "run",
                                    return_value=mock.Mock(returncode=0)), \
                 mock.patch.dict(os.environ, {"COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13"}), \
                 redirect_stdout(out):
                rc = coverage_gate.main()
            self.assertEqual(rc, 0)
            self.assertEqual(coverage_baseline.load_baseline(tmp), set())

    def test_update_baseline_never_blesses_a_net_new_gap(self):
        with tempfile.TemporaryDirectory() as tmp:
            coverage_baseline.write_baseline(tmp, set())
            summary_path = os.path.join(tmp, "gcovr-summary.json")
            coveralls_path = os.path.join(tmp, "gcovr-coveralls.json")
            detail_path = os.path.join(tmp, "gcovr-detail.json")
            with open(summary_path, "w", encoding="utf-8") as fh:
                json.dump(_NONZERO_SUMMARY, fh)
            with open(detail_path, "w", encoding="utf-8") as fh:
                json.dump({"files": [{"file": "a.c", "lines": [
                    {"line_number": 5, "count": 0, "branches": []},
                ]}]}, fh)
            argv = [
                "coverage_gate.py", "--root", tmp,
                "--summary-out", summary_path, "--coveralls-out", coveralls_path,
                "--detail-out", detail_path, "--update-baseline",
            ]
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(coverage_gate.subprocess, "run",
                                    return_value=mock.Mock(returncode=0)), \
                 mock.patch.dict(os.environ, {"COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13"}), \
                 redirect_stdout(io.StringIO()):
                rc = coverage_gate.main()
            self.assertEqual(rc, 0)  # update-baseline itself always succeeds
            self.assertEqual(coverage_baseline.load_baseline(tmp), set())

    def test_seed_and_update_baseline_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            with mock.patch.object(
                sys, "argv",
                ["coverage_gate.py", "--seed-baseline", "--update-baseline"],
            ):
                coverage_gate.main()


if __name__ == "__main__":
    unittest.main()
