"""Tests for scripts/coverage_baseline.py: the B1-764 shrink-only per-line
coverage ratchet (Marker/diff/baseline engine reused from
scripts/bbtool/fence/_base.py), extended by B1-1258 to also ratchet
per-line branch-existence gaps -- branch-EDGE ids are still deliberately
never baselined (not stable across gcc majors, see module docstring); only
"this line has an uncovered branch" (keyed by line number, same as
uncovered_line) is."""
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

    def test_uncovered_branch_produces_a_line_keyed_marker(self):
        """B1-1258: an uncovered branch produces an uncovered_branch marker
        keyed by LINE NUMBER only -- never source_block_id/
        destination_block_id, which are not stable across gcc majors (a
        dev machine's Homebrew gcc-16 vs CI's ubuntu-latest stock gcc split
        the identical line's compound conditional differently). Keying on
        the line number instead sidesteps that instability."""
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=3, branches=[_branch(2, 3, count=0)]),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, {Marker("uncovered_branch", "a.c", "5")})

    def test_covered_branches_produce_no_marker(self):
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=3, branches=[_branch(2, 3, count=1), _branch(2, 4, count=2)]),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, set())

    def test_multiple_uncovered_branches_on_one_line_collapse_to_one_marker(self):
        """One marker per LINE regardless of how many of its branch edges
        are uncovered or how gcc splits them -- the marker means 'this line
        has at least one not-fully-exercised branch', not 'branch N is
        uncovered'."""
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=3, branches=[_branch(2, 3, count=0), _branch(2, 4, count=0)]),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, {Marker("uncovered_branch", "a.c", "5")})

    def test_excluded_branch_on_a_covered_line_produces_no_marker(self):
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=3, branches=[_branch(2, 3, count=0, excluded=True)]),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, set())

    def test_excluded_line_suppresses_its_branch_markers_too(self):
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=0, branches=[_branch(2, 3, count=0)], excluded=True),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, set())

    def test_uncovered_line_and_uncovered_branch_on_the_same_line_both_appear(self):
        detail = {"files": [{"file": "a.c", "lines": [
            _line(5, count=0, branches=[_branch(2, 3, count=0)]),
        ]}]}
        markers = coverage_baseline.build_markers(detail)
        self.assertEqual(markers, {
            Marker("uncovered_line", "a.c", "5"),
            Marker("uncovered_branch", "a.c", "5"),
        })

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

    def test_identity_is_type_sensitive(self):
        """An uncovered_line and an uncovered_branch on the same line/file
        must never collide -- they are independent gaps that can close
        independently."""
        m1 = Marker("uncovered_line", "a.c", "5")
        m2 = Marker("uncovered_branch", "a.c", "5")
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

    def test_ignore_types_excludes_that_type_from_new_and_removed_entirely(self):
        """B1-1258: the CI COVERAGE_GATE_LINES_ONLY opt-out path -- with
        ignore_types={"uncovered_branch"}, a branch baseline entry absent
        from `current` (because current was pre-filtered to lines-only)
        must NOT be reported as a prune candidate (it wasn't verified
        fixed, just unmeasured this run), and a branch entry present in
        `current` but not baseline must still not fail."""
        baseline = {
            Marker("uncovered_line", "a.c", "5"),
            Marker("uncovered_branch", "a.c", "10"),
        }
        coverage_baseline.write_baseline(self.root, baseline)
        current = {Marker("uncovered_line", "a.c", "5")}  # branch marker absent (filtered)
        out = io.StringIO()
        with redirect_stdout(out):
            ok = coverage_baseline.check(self.root, current, frozenset({"uncovered_branch"}))
        self.assertTrue(ok)
        self.assertNotIn("candidate to prune", out.getvalue())

    def test_ignore_types_still_fails_on_a_genuine_uncovered_line_regression(self):
        baseline = {Marker("uncovered_branch", "a.c", "10")}
        coverage_baseline.write_baseline(self.root, baseline)
        current = {Marker("uncovered_line", "a.c", "5")}  # new line gap, unbaselined
        err = io.StringIO()
        with redirect_stderr(err):
            ok = coverage_baseline.check(self.root, current, frozenset({"uncovered_branch"}))
        self.assertFalse(ok)
        self.assertIn("a.c", err.getvalue())


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

    def test_seed_with_no_baseline_and_no_gaps_errors(self):
        """B1-1258: seed() itself now raises (caller used to precheck via
        baseline_path().is_file() -- see coverage_gate.py's --seed-baseline
        wiring)."""
        with self.assertRaises(coverage_baseline.SeedError):
            coverage_baseline.seed(self.root, set())

    def test_seed_a_type_already_baselined_with_no_new_type_errors(self):
        coverage_baseline.write_baseline(self.root, {Marker("uncovered_line", "a.c", "5")})
        with self.assertRaises(coverage_baseline.SeedError):
            coverage_baseline.seed(self.root, {Marker("uncovered_line", "a.c", "99")})
        # unchanged -- the attempted reseed of an already-baselined type never wrote
        self.assertEqual(
            coverage_baseline.load_baseline(self.root),
            {Marker("uncovered_line", "a.c", "5")},
        )

    def test_seed_adds_a_genuinely_new_type_alongside_an_existing_baseline(self):
        """The B1-1258 one-time move: an uncovered_line baseline already
        exists (from the original B1-764 seed); seeding again with
        uncovered_branch markers present in `current` merges them in
        without touching the existing uncovered_line entries."""
        coverage_baseline.write_baseline(self.root, {Marker("uncovered_line", "a.c", "5")})
        current = {
            Marker("uncovered_line", "a.c", "5"),
            Marker("uncovered_branch", "b.c", "10"),
        }
        coverage_baseline.seed(self.root, current)
        self.assertEqual(coverage_baseline.load_baseline(self.root), current)

    def test_seed_ignores_new_gaps_of_an_already_baselined_type(self):
        """Seeding a new type must never smuggle in a net-new gap of an
        already-baselined type -- that always requires --update-baseline
        (shrink-only) or a deliberate reviewed baseline edit, never a
        reseed."""
        coverage_baseline.write_baseline(self.root, {Marker("uncovered_line", "a.c", "5")})
        current = {
            Marker("uncovered_line", "a.c", "5"),
            Marker("uncovered_line", "a.c", "99"),  # new -- must NOT be added
            Marker("uncovered_branch", "b.c", "10"),
        }
        coverage_baseline.seed(self.root, current)
        self.assertEqual(
            coverage_baseline.load_baseline(self.root),
            {Marker("uncovered_line", "a.c", "5"), Marker("uncovered_branch", "b.c", "10")},
        )


if __name__ == "__main__":
    unittest.main()
