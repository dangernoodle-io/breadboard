"""Tests for scripts/coverage_gate.py: the zero-total/zero-percent tooling-
failure guard (B1-867), the B1-764 coverage-baseline ratchet wiring
(--seed-baseline / --update-baseline / default check), and the B1-1258
COVERAGE_GATE_LINES_ONLY CI opt-out (branch ratcheting is a local-only aid;
CI never sees uncovered_branch markers, only uncovered_line)."""
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
                 return_value=mock.Mock(returncode=0)) as run_mock, \
             mock.patch.dict(os.environ, env), \
             redirect_stdout(out), redirect_stderr(err):
            rc = coverage_gate.main()
        self.gcovr_cmd = run_mock.call_args[0][0] if run_mock.call_args else None
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


class TestGcovrTraversalExcludes(_CoverageGateTestBase):
    """B1-1264: the gcovr argv is assembled in code and is otherwise only
    line-covered, so a dropped --exclude-directories entry would pass every
    test and surface instead as an environment-dependent gcovr abort (a stray
    .gcda from another compiler major under .worktrees/ kills the whole run).
    Assert flag/value adjacency, not mere membership."""

    def _exclude_pairs(self):
        self._run(_NONZERO_SUMMARY)
        cmd = self.gcovr_cmd
        self.assertIsNotNone(cmd, "gcovr was never invoked")
        return [(cmd[i], cmd[i + 1]) for i in range(len(cmd) - 1)]

    def test_dot_worktrees_is_excluded_from_traversal(self):
        self.assertIn(("--exclude-directories", r"\.worktrees"),
                      self._exclude_pairs())

    def test_dot_claude_is_excluded_from_traversal(self):
        self.assertIn(("--exclude-directories", r"\.claude"),
                      self._exclude_pairs())


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

    def test_new_uncovered_branch_with_100_percent_line_coverage_fails(self):
        """The PR #1120 scenario: a brand-new file at 100% LINE coverage
        with an uncovered branch used to false-pass this gate (branch
        markers weren't baselined at all) while Coveralls's aggregate
        branch-percent gate correctly failed it. B1-1258 closes that gap."""
        detail = {"files": [{"file": "components/bb_wifi_prov/src/bb_wifi_prov_page.c", "lines": [
            {"line_number": 37, "count": 1, "branches": [
                {"source_block_id": 0, "destination_block_id": 1, "count": 0},
            ]},
        ]}]}
        rc, _out, err, _tmp = self._run(_NONZERO_SUMMARY, detail=detail)
        self.assertEqual(rc, 1)
        self.assertIn("bb_wifi_prov_page.c", err)
        self.assertIn("uncovered_branch", err)
        self.assertIn("FAIL", err)

    def test_excluded_line_never_becomes_a_baseline_gap(self):
        detail = {"files": [{"file": "a.c", "lines": [
            {"line_number": 5, "count": 0, "branches": [], "gcovr/excluded": True},
        ]}]}
        rc, out, _err, _tmp = self._run(_NONZERO_SUMMARY, detail=detail)
        self.assertEqual(rc, 0)
        self.assertIn("PASS", out)


class TestLinesOnlyEnabled(unittest.TestCase):
    """B1-1258: coverage_gate.lines_only_enabled() -- the predicate the CI
    opt-out is built on. Never auto-detects a generic $CI-style var; only
    the explicit COVERAGE_GATE_LINES_ONLY name is honored."""

    def test_unset_is_disabled(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            os.environ.pop(coverage_gate.LINES_ONLY_ENV, None)
            self.assertFalse(coverage_gate.lines_only_enabled())

    def test_generic_ci_var_does_not_enable_it(self):
        """The whole point of an explicit opt-out: a container/CI runner
        that happens to export the generic $CI=true var must NOT silently
        change local behavior -- only COVERAGE_GATE_LINES_ONLY does."""
        with mock.patch.dict(os.environ, {"CI": "true"}, clear=True):
            self.assertFalse(coverage_gate.lines_only_enabled())

    def test_falsy_values_are_disabled(self):
        for val in ("0", "false", "False"):
            with mock.patch.dict(os.environ, {coverage_gate.LINES_ONLY_ENV: val}, clear=True):
                self.assertFalse(coverage_gate.lines_only_enabled(), msg=val)

    def test_truthy_values_are_enabled(self):
        for val in ("1", "true", "yes"):
            with mock.patch.dict(os.environ, {coverage_gate.LINES_ONLY_ENV: val}, clear=True):
                self.assertTrue(coverage_gate.lines_only_enabled(), msg=val)


class TestLinesOnlyCIOptOutWiring(_CoverageGateTestBase):
    """B1-1258: end-to-end through main() -- with COVERAGE_GATE_LINES_ONLY
    set, an uncovered_branch marker must NOT fail the gate, while an
    uncovered_line marker on the same run still must."""

    _DETAIL_LINE_AND_BRANCH_GAP = {"files": [{"file": "a.c", "lines": [
        {"line_number": 5, "count": 0, "branches": []},
        {"line_number": 10, "count": 3, "branches": [
            {"source_block_id": 0, "destination_block_id": 1, "count": 0},
        ]},
    ]}]}

    def test_without_the_env_var_both_line_and_branch_gaps_fail(self):
        rc, _out, err, _tmp = self._run(
            _NONZERO_SUMMARY, detail=self._DETAIL_LINE_AND_BRANCH_GAP,
        )
        self.assertEqual(rc, 1)
        self.assertIn("uncovered_line", err)
        self.assertIn("uncovered_branch", err)

    def test_without_the_env_var_the_aggregate_line_claims_both_ratcheted(self):
        rc, out, _err, _tmp = self._run(_NONZERO_SUMMARY, detail={"files": []})
        self.assertEqual(rc, 0)
        self.assertIn("both LINE and per-line-BRANCH-existence markers are "
                       "per-marker ratcheted", out)

    def test_with_the_env_var_the_aggregate_line_never_claims_branches_are_ratcheted(self):
        """The B1-1258 re-review nit: the aggregate summary print must not
        assert branch ratcheting is in effect for a run where it isn't --
        even in isolation (e.g. someone grepping just that one line out of
        a CI log)."""
        rc, out, _err, _tmp = self._run(
            _NONZERO_SUMMARY, detail={"files": []},
            extra_env={coverage_gate.LINES_ONLY_ENV: "1"},
        )
        self.assertEqual(rc, 0)
        self.assertIn("LINES ONLY are per-marker ratcheted", out)
        self.assertNotIn("both LINE and per-line-BRANCH-existence markers are "
                          "per-marker ratcheted", out)

    def test_with_the_env_var_only_the_line_gap_fails(self):
        rc, _out, err, _tmp = self._run(
            _NONZERO_SUMMARY, detail=self._DETAIL_LINE_AND_BRANCH_GAP,
            extra_env={coverage_gate.LINES_ONLY_ENV: "1"},
        )
        self.assertEqual(rc, 1)
        self.assertIn("uncovered_line", err)
        self.assertNotIn("uncovered_branch", err)

    def test_with_the_env_var_a_branch_only_gap_passes(self):
        detail = {"files": [{"file": "a.c", "lines": [
            {"line_number": 10, "count": 3, "branches": [
                {"source_block_id": 0, "destination_block_id": 1, "count": 0},
            ]},
        ]}]}
        rc, out, _err, _tmp = self._run(
            _NONZERO_SUMMARY, detail=detail,
            extra_env={coverage_gate.LINES_ONLY_ENV: "1"},
        )
        self.assertEqual(rc, 0)
        self.assertIn("PASS", out)
        self.assertIn(coverage_gate.LINES_ONLY_ENV, out)

    def test_env_var_does_not_smuggle_a_branch_gap_into_the_seeded_baseline(self):
        """A --seed-baseline run under the CI opt-out must never write
        uncovered_branch entries into the committed baseline -- seeding is a
        manual, local-only operation, but this guards the wiring itself."""
        detail = {"files": [{"file": "a.c", "lines": [
            {"line_number": 5, "count": 0, "branches": []},
            {"line_number": 10, "count": 3, "branches": [
                {"source_block_id": 0, "destination_block_id": 1, "count": 0},
            ]},
        ]}]}
        rc, _out, _err, tmp = self._run(
            _NONZERO_SUMMARY, detail=detail, extra_args=["--seed-baseline"],
            extra_env={coverage_gate.LINES_ONLY_ENV: "1"},
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            coverage_baseline.load_baseline(tmp),
            {Marker("uncovered_line", "a.c", "5")},
        )

    def test_env_var_suppresses_spurious_branch_prune_noise_against_a_real_baseline(self):
        """The real-world shape: a committed baseline already has
        uncovered_branch entries (the 501-marker B1-1258 seed). Under the CI
        opt-out, `current` never contains any uncovered_branch marker (CI's
        gcc never even gets asked), so a naive diff would report every one
        of those 501 as a "candidate to prune" -- misleading log noise
        implying they were verified fixed, when they were simply never
        measured this run. check()'s ignore_types must suppress that."""
        with tempfile.TemporaryDirectory() as tmp:
            coverage_baseline.write_baseline(tmp, {
                Marker("uncovered_line", "a.c", "5"),
                Marker("uncovered_branch", "b.c", "10"),
            })
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
                 mock.patch.dict(os.environ, {
                     "COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13",
                     coverage_gate.LINES_ONLY_ENV: "1",
                 }), \
                 redirect_stdout(out):
                rc = coverage_gate.main()
            self.assertEqual(rc, 0)
            self.assertIn("PASS", out.getvalue())
            self.assertNotIn("candidate to prune", out.getvalue())


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
        """B1-1258: seed() (not a caller-side file-exists precheck) is what
        raises now -- a second seed with nothing new to add (same, empty,
        detail) still errors the same way."""
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

    def test_seed_baseline_adds_uncovered_branch_alongside_existing_line_baseline(self):
        """B1-1258: the real one-time migration path -- a baseline already
        seeded with only uncovered_line entries (the original B1-764 seed)
        can be re-run through --seed-baseline once to additively bring in
        uncovered_branch entries, without touching the existing lines."""
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
                    {"line_number": 10, "count": 3, "branches": [
                        {"source_block_id": 1, "destination_block_id": 2, "count": 0},
                    ]},
                ]}]}, fh)
            argv = [
                "coverage_gate.py", "--root", tmp,
                "--summary-out", summary_path, "--coveralls-out", coveralls_path,
                "--detail-out", detail_path, "--seed-baseline",
            ]
            out = io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(coverage_gate.subprocess, "run",
                                    return_value=mock.Mock(returncode=0)), \
                 mock.patch.dict(os.environ, {"COVERAGE_RESOLVED_GCOV": "/usr/bin/gcov-13"}), \
                 redirect_stdout(out):
                rc = coverage_gate.main()
            self.assertEqual(rc, 0)
            self.assertIn("seeded", out.getvalue())
            self.assertEqual(
                coverage_baseline.load_baseline(tmp),
                {Marker("uncovered_line", "a.c", "5"), Marker("uncovered_branch", "a.c", "10")},
            )

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
