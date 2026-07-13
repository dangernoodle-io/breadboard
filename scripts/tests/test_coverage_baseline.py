"""Tests for scripts/coverage_baseline.py: the B1-764 shrink-only per-line
coverage ratchet (Marker/diff/baseline engine reused from
scripts/bbtool/fence/_base.py). LINES ONLY -- branch edges are deliberately
never baselined (not stable across gcc majors, see module docstring)."""
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import coverage_baseline  # noqa: E402
from fence._base import Marker  # noqa: E402


def _line(line_number, count, branches=None, excluded=False):
    entry = {"line_number": line_number, "count": count, "branches": branches or []}
    if excluded:
        entry["gcovr/excluded"] = True
    return entry


def _branch(src, dst, count, excluded=False):
    entry = {"source_block_id": src, "destination_block_id": dst, "count": count}
    if excluded:
        entry["gcovr/excluded"] = True
    return entry


class TestBuildMarkers(unittest.TestCase):
    def test_covered_line_produces_no_marker(self):
        detail = {"files": [{"file": "a.c", "lines": [_line(5, count=3)]}]}
        self.assertEqual(coverage_baseline.build_markers(detail), set())

    def test_uncovered_line_produces_a_marker(self):
        detail = {"files": [{"file": "a.c", "lines": [_line(5, count=0)]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, {Marker("uncovered_line", "a.c", "5")})

    def test_uncovered_branch_produces_no_marker(self):
        """Branch edges are deliberately never baselined -- gcov's
        source_block_id/destination_block_id pairs are not stable across gcc
        majors (a dev machine's Homebrew gcc-16 vs CI's ubuntu-latest stock
        gcc split the identical line's compound conditional differently),
        so a per-branch marker would spuriously flag untouched code as
        'new' the moment CI's gcc major differs from the one that seeded
        the baseline. Branch coverage is still measured/reported at the
        aggregate level (see coverage_gate.py) -- just not per-marker
        ratcheted."""
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=3, branches=[_branch(2, 3, count=0)]),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, set())

    def test_excluded_line_is_never_a_marker_even_at_zero_count(self):
        """B1-871-adjacent: a gcovr-excluded line (LCOV_EXCL_LINE) must not
        be baselined as a false gap -- gcovr itself already drops it from
        line_percent."""
        detail = {"files": [{"file": "a.c", "lines": [_line(5, count=0, excluded=True)]}]}
        self.assertEqual(coverage_baseline.build_markers(detail), set())

    def test_multiple_files_and_mixed_gaps(self):
        detail = {"files": [
            {"file": "a.c", "lines": [_line(1, count=0)]},
            {"file": "b.c", "lines": [_line(1, count=5)]},
        ]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, {Marker("uncovered_line", "a.c", "1")})


class TestIdentity(unittest.TestCase):
    def test_identity_is_path_sensitive(self):
        """Unlike di_legacy's default (type, id) identity, two different
        files with an uncovered line "5" must never collide -- a bare line
        number is not globally unique the way a macro/component name is."""
        m1 = Marker("uncovered_line", "a.c", "5")
        m2 = Marker("uncovered_line", "b.c", "5")
        self.assertNotEqual(coverage_baseline.identity(m1), coverage_baseline.identity(m2))


class _BaselineFileTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()


class TestLoadWriteBaseline(_BaselineFileTestBase):
    def test_load_missing_baseline_is_empty_set(self):
        self.assertEqual(coverage_baseline.load_baseline(self.root), set())

    def test_write_then_load_round_trips(self):
        """The write/load engine itself is type-agnostic (reused from
        fence._base) -- it round-trips whatever Marker types it's given,
        even though build_markers() only ever produces uncovered_line now."""
        markers = {Marker("uncovered_line", "a.c", "5"), Marker("uncovered_line", "a.c", "6")}
        coverage_baseline.write_baseline(self.root, markers)
        self.assertEqual(coverage_baseline.load_baseline(self.root), markers)

    def test_baseline_file_lives_under_dot_baseline_coverage(self):
        """Deliberately NOT under .baseline/bbtool/fence/ -- see module
        docstring for why this family is not bbtool-fence-auto-discovered."""
        path = coverage_baseline.baseline_path(self.root)
        self.assertEqual(path, Path(self.root) / ".baseline" / "coverage" / "coverage.json")


class TestCheck(_BaselineFileTestBase):
    def test_no_baseline_and_no_markers_passes(self):
        out = io.StringIO()
        with redirect_stdout(out):
            ok = coverage_baseline.check(self.root, set())
        self.assertTrue(ok)
        self.assertIn("PASS", out.getvalue())

    def test_new_file_with_any_gap_fails_even_with_no_baseline(self):
        """A brand-new file has zero baseline entries -- any uncovered line
        in it is unconditionally 'new', holding new code to 100%."""
        current = {Marker("uncovered_line", "new_file.c", "10")}
        err = io.StringIO()
        with redirect_stderr(err):
            ok = coverage_baseline.check(self.root, current)
        self.assertFalse(ok)
        self.assertIn("new_file.c", err.getvalue())
        self.assertIn("FAIL", err.getvalue())

    def test_baselined_gap_passes(self):
        baseline = {Marker("uncovered_line", "a.c", "5")}
        coverage_baseline.write_baseline(self.root, baseline)
        out = io.StringIO()
        with redirect_stdout(out):
            ok = coverage_baseline.check(self.root, baseline)
        self.assertTrue(ok)

    def test_regression_a_baselined_line_now_covered_but_a_different_line_now_uncovered_fails(self):
        """Simulates the line-shift-on-edit case: the old gap closed, but a
        different (new) gap opened -- still an overall FAIL."""
        baseline = {Marker("uncovered_line", "a.c", "5")}
        coverage_baseline.write_baseline(self.root, baseline)
        current = {Marker("uncovered_line", "a.c", "6")}
        err = io.StringIO()
        with redirect_stderr(err):
            ok = coverage_baseline.check(self.root, current)
        self.assertFalse(ok)
        self.assertIn("a.c", err.getvalue())

    def test_prune_candidate_is_informational_not_a_failure(self):
        baseline = {Marker("uncovered_line", "a.c", "5")}
        coverage_baseline.write_baseline(self.root, baseline)
        out = io.StringIO()
        with redirect_stdout(out):
            ok = coverage_baseline.check(self.root, set())
        self.assertTrue(ok)
        self.assertIn("candidate to prune", out.getvalue())


class TestUpdateBaseline(_BaselineFileTestBase):
    def test_prunes_now_covered_entries(self):
        baseline = {
            Marker("uncovered_line", "a.c", "5"),
            Marker("uncovered_line", "a.c", "6"),
        }
        coverage_baseline.write_baseline(self.root, baseline)
        current = {Marker("uncovered_line", "a.c", "5")}  # line 6 now covered
        coverage_baseline.update_baseline(self.root, current)
        self.assertEqual(coverage_baseline.load_baseline(self.root), current)

    def test_never_adds_a_net_new_marker(self):
        """--update-baseline is shrink-only: a genuinely new gap must never
        be silently blessed into the baseline."""
        baseline = {Marker("uncovered_line", "a.c", "5")}
        coverage_baseline.write_baseline(self.root, baseline)
        current = baseline | {Marker("uncovered_line", "a.c", "99")}
        coverage_baseline.update_baseline(self.root, current)
        self.assertEqual(coverage_baseline.load_baseline(self.root), baseline)


class TestSeed(_BaselineFileTestBase):
    def test_seed_writes_the_current_set_wholesale(self):
        current = {Marker("uncovered_line", "a.c", "5")}
        coverage_baseline.seed(self.root, current)
        self.assertEqual(coverage_baseline.load_baseline(self.root), current)


if __name__ == "__main__":
    unittest.main()
